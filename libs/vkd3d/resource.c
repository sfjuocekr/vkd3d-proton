/*
 * Copyright 2016 Józef Kucia for CodeWeavers
 * Copyright 2019 Conor McCarthy for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#include <float.h>

#include "vkd3d_private.h"
#include "vkd3d_rw_spinlock.h"
#include "vkd3d_descriptor_debug.h"
#include "hashmap.h"

#define VKD3D_NULL_SRV_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM
#define VKD3D_NULL_UAV_FORMAT DXGI_FORMAT_R32_UINT

static LONG64 global_cookie_counter;

LONG64 vkd3d_allocate_cookie()
{
    return InterlockedIncrement64(&global_cookie_counter);
}

static VkImageType vk_image_type_from_d3d12_resource_dimension(D3D12_RESOURCE_DIMENSION dimension)
{
    switch (dimension)
    {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            return VK_IMAGE_TYPE_1D;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            return VK_IMAGE_TYPE_2D;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            return VK_IMAGE_TYPE_3D;
        default:
            ERR("Invalid resource dimension %#x.\n", dimension);
            return VK_IMAGE_TYPE_2D;
    }
}

VkSampleCountFlagBits vk_samples_from_sample_count(unsigned int sample_count)
{
    switch (sample_count)
    {
        case 1:
            return VK_SAMPLE_COUNT_1_BIT;
        case 2:
            return VK_SAMPLE_COUNT_2_BIT;
        case 4:
            return VK_SAMPLE_COUNT_4_BIT;
        case 8:
            return VK_SAMPLE_COUNT_8_BIT;
        case 16:
            return VK_SAMPLE_COUNT_16_BIT;
        case 32:
            return VK_SAMPLE_COUNT_32_BIT;
        case 64:
            return VK_SAMPLE_COUNT_64_BIT;
        default:
            return 0;
    }
}

VkSampleCountFlagBits vk_samples_from_dxgi_sample_desc(const DXGI_SAMPLE_DESC *desc)
{
    VkSampleCountFlagBits vk_samples;

    if ((vk_samples = vk_samples_from_sample_count(desc->Count)))
        return vk_samples;

    FIXME("Unhandled sample count %u.\n", desc->Count);
    return VK_SAMPLE_COUNT_1_BIT;
}

HRESULT vkd3d_create_buffer(struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc, VkBuffer *vk_buffer)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkExternalMemoryBufferCreateInfo external_info;
    const bool sparse_resource = !heap_properties;
    VkBufferCreateInfo buffer_info;
    D3D12_HEAP_TYPE heap_type;
    VkResult vr;

    heap_type = heap_properties ? heap_properties->Type : D3D12_HEAP_TYPE_DEFAULT;

    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.pNext = NULL;
    buffer_info.flags = 0;
    buffer_info.size = desc->Width;

    /* This is only used by OpenExistingHeapFrom*,
     * and external host memory is the only way for us to do CROSS_ADAPTER. */
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER)
    {
        external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        external_info.pNext = NULL;
        external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        buffer_info.pNext = &external_info;
    }

    if (sparse_resource)
    {
        buffer_info.flags |= VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT |
                VK_BUFFER_CREATE_SPARSE_ALIASED_BIT;
    }

    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT
            | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
            | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
            | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
            | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    if (device->vk_info.EXT_conditional_rendering)
        buffer_info.usage |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;

    if (heap_type == D3D12_HEAP_TYPE_DEFAULT && device->vk_info.EXT_transform_feedback)
    {
        buffer_info.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT
                | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
    }

    if (d3d12_device_supports_ray_tracing_tier_1_0(device))
    {
        /* Allows us to place GENERIC acceleration structures on top of VkBuffers.
         * This should only be allowed on non-host visible heaps. UPLOAD / READBACK is banned
         * because of resource state rules, but CUSTOM might be allowed, needs to be verified. */
        if (heap_type == D3D12_HEAP_TYPE_DEFAULT || !is_cpu_accessible_heap(heap_properties))
            buffer_info.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
        /* This is always allowed. Used for vertex/index buffer inputs to RTAS build. */
        buffer_info.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    if (heap_type == D3D12_HEAP_TYPE_UPLOAD)
        buffer_info.usage &= ~VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    else if (heap_type == D3D12_HEAP_TYPE_READBACK)
    {
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }

    if (device->device_info.buffer_device_address_features.bufferDeviceAddress)
        buffer_info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR;

    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        buffer_info.usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

    if (!(desc->Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        buffer_info.usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;

    /* Buffers always have properties of D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS. */
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
    {
        WARN("D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS cannot be set for buffers.\n");
        return E_INVALIDARG;
    }

    if (device->queue_family_count > 1)
    {
        buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        buffer_info.queueFamilyIndexCount = device->queue_family_count;
        buffer_info.pQueueFamilyIndices = device->queue_family_indices;
    }
    else
    {
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        buffer_info.queueFamilyIndexCount = 0;
        buffer_info.pQueueFamilyIndices = NULL;
    }

    if (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        FIXME("Unsupported resource flags %#x.\n", desc->Flags);

    if ((vr = VK_CALL(vkCreateBuffer(device->vk_device, &buffer_info, NULL, vk_buffer))) < 0)
    {
        WARN("Failed to create Vulkan buffer, vr %d.\n", vr);
        *vk_buffer = VK_NULL_HANDLE;
    }

    return hresult_from_vk_result(vr);
}

static unsigned int max_miplevel_count(const D3D12_RESOURCE_DESC *desc)
{
    unsigned int size = max(desc->Width, desc->Height);
    size = max(size, d3d12_resource_desc_get_depth(desc, 0));
    return vkd3d_log2i(size) + 1;
}

static const struct vkd3d_format_compatibility_list *vkd3d_get_format_compatibility_list(
        const struct d3d12_device *device, DXGI_FORMAT dxgi_format)
{
    DXGI_FORMAT typeless_format;
    unsigned int i;

    if (!(typeless_format = vkd3d_get_typeless_format(device, dxgi_format)))
        typeless_format = dxgi_format;

    for (i = 0; i < device->format_compatibility_list_count; ++i)
    {
        if (device->format_compatibility_lists[i].typeless_format == typeless_format)
            return &device->format_compatibility_lists[i];
    }

    return NULL;
}

static bool vkd3d_is_linear_tiling_supported(const struct d3d12_device *device, VkImageCreateInfo *image_info)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkImageFormatProperties properties;
    bool supported;
    VkResult vr;

    if ((vr = VK_CALL(vkGetPhysicalDeviceImageFormatProperties(device->vk_physical_device, image_info->format,
            image_info->imageType, VK_IMAGE_TILING_LINEAR, image_info->usage, image_info->flags, &properties))) < 0)
    {
        if (vr != VK_ERROR_FORMAT_NOT_SUPPORTED)
            WARN("Failed to get device image format properties, vr %d.\n", vr);
        else
        {
            WARN("Attempting to create linear image, but not supported.\n"
                  "usage: %#x, flags: %#x, fmt: %u, image_type: %u\n",
                  image_info->usage, image_info->flags, image_info->format, image_info->imageType);
        }

        return false;
    }

    supported = image_info->extent.depth <= properties.maxExtent.depth
            && image_info->mipLevels <= properties.maxMipLevels
            && image_info->arrayLayers <= properties.maxArrayLayers
            && (image_info->samples & properties.sampleCounts);

    if (!supported)
    {
        WARN("Linear tiling not supported for mipLevels = %u, arrayLayers = %u, sampes = %u, depth = %u.\n",
                image_info->mipLevels, image_info->arrayLayers, image_info->samples, image_info->extent.depth);
    }

    return supported;
}

static VkImageLayout vk_common_image_layout_from_d3d12_desc(const D3D12_RESOURCE_DESC *desc)
{
    /* We need aggressive decay and promotion into anything. */
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
        return VK_IMAGE_LAYOUT_GENERAL;
    if (desc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
        return VK_IMAGE_LAYOUT_GENERAL;

    /* DENY_SHADER_RESOURCE only allowed with ALLOW_DEPTH_STENCIL */
    if (desc->Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static bool vkd3d_sparse_image_may_have_mip_tail(const D3D12_RESOURCE_DESC *desc,
        const VkSparseImageFormatProperties *sparse_info)
{
    VkExtent3D mip_extent, block_extent = sparse_info->imageGranularity;
    unsigned int mip_level;

    /* probe smallest mip level in the image */
    mip_level = desc->MipLevels - 1;
    mip_extent.width = d3d12_resource_desc_get_width(desc, mip_level);
    mip_extent.height = d3d12_resource_desc_get_height(desc, mip_level);
    mip_extent.depth = d3d12_resource_desc_get_depth(desc, mip_level);

    if (sparse_info->flags & VK_SPARSE_IMAGE_FORMAT_ALIGNED_MIP_SIZE_BIT)
    {
        return mip_extent.width % block_extent.width ||
                mip_extent.height % block_extent.height ||
                mip_extent.depth % block_extent.depth;
    }

    return mip_extent.width < block_extent.width ||
            mip_extent.height < block_extent.height ||
            mip_extent.depth < block_extent.depth;
}

static bool vkd3d_resource_can_be_vrs(struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, const D3D12_RESOURCE_DESC *desc)
{
    return device->device_info.fragment_shading_rate_features.attachmentFragmentShadingRate &&
            desc->Format == DXGI_FORMAT_R8_UINT &&
            desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            desc->MipLevels == 1 &&
            desc->SampleDesc.Count == 1 &&
            desc->SampleDesc.Quality == 0 &&
            desc->Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
            heap_properties &&
            !is_cpu_accessible_heap(heap_properties) &&
            !(desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
                D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY));
}

static HRESULT vkd3d_resource_make_vrs_view(struct d3d12_device *device,
        VkImage image, VkImageView* view)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkImageViewCreateInfo view_info;
    VkResult vr;

    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = NULL;
    view_info.flags = 0;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8_UINT;
    view_info.components = (VkComponentMapping) {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
    };
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if ((vr = VK_CALL(vkCreateImageView(device->vk_device, &view_info, NULL, view))) < 0)
        ERR("Failed to create implicit VRS view, vr %d.\n", vr);

    return hresult_from_vk_result(vr);
}

static HRESULT vkd3d_create_image(struct d3d12_device *device,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags,
        const D3D12_RESOURCE_DESC *desc, struct d3d12_resource *resource, VkImage *vk_image)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    const struct vkd3d_format_compatibility_list *compat_list;
    const bool sparse_resource = !heap_properties;
    VkImageFormatListCreateInfoKHR format_list;
    const struct vkd3d_format *format;
    VkImageCreateInfo image_info;
    DXGI_FORMAT typeless_format;
    bool use_concurrent;
    unsigned int i;
    VkResult vr;

    if (!resource)
    {
        if (!(format = vkd3d_format_from_d3d12_resource_desc(device, desc, 0)))
        {
            WARN("Invalid DXGI format %#x.\n", desc->Format);
            return E_INVALIDARG;
        }
    }
    else
    {
        format = resource->format;
    }

    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = NULL;
    image_info.flags = 0;
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    {
        /* Format compatibility rules are more relaxed for UAVs. */
        if (format->type != VKD3D_FORMAT_TYPE_UINT)
            image_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    }
    else if (!(desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && format->type == VKD3D_FORMAT_TYPE_TYPELESS)
    {
        image_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;

        if ((compat_list = vkd3d_get_format_compatibility_list(device, desc->Format)))
        {
            format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
            format_list.pNext = NULL;
            format_list.viewFormatCount = compat_list->format_count;
            format_list.pViewFormats = compat_list->vk_formats;

            image_info.pNext = &format_list;
        }
    }
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
            && desc->Width == desc->Height && desc->DepthOrArraySize >= 6
            && desc->SampleDesc.Count == 1)
        image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        image_info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT_KHR;

    if (sparse_resource)
    {
        image_info.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;

        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
        {
            WARN("Tiled 1D textures not supported.\n");
            return E_INVALIDARG;
        }

        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
                device->d3d12_caps.options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_3)
        {
            WARN("Tiled 3D textures not supported by device.\n");
            return E_INVALIDARG;
        }

        if (!is_power_of_two(format->vk_aspect_mask))
        {
            WARN("Multi-planar format %u not supported for tiled resources.\n", desc->Format);
            return E_INVALIDARG;
        }
    }

    image_info.imageType = vk_image_type_from_d3d12_resource_dimension(desc->Dimension);
    image_info.format = format->vk_format;
    image_info.extent.width = desc->Width;
    image_info.extent.height = desc->Height;

    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    {
        image_info.extent.depth = desc->DepthOrArraySize;
        image_info.arrayLayers = 1;
    }
    else
    {
        image_info.extent.depth = 1;
        image_info.arrayLayers = desc->DepthOrArraySize;
    }

    image_info.mipLevels = min(desc->MipLevels, max_miplevel_count(desc));
    image_info.samples = vk_samples_from_dxgi_sample_desc(&desc->SampleDesc);

    if (sparse_resource)
    {
        if (desc->Layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE)
        {
            WARN("D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE must be used for reserved texture.\n");
            return E_INVALIDARG;
        }

        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    }
    else if (desc->Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN)
    {
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    }
    else if (desc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
    {
        image_info.tiling = VK_IMAGE_TILING_LINEAR;
    }
    else
    {
        FIXME("Unsupported layout %#x.\n", desc->Layout);
        return E_NOTIMPL;
    }

    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
        image_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
        image_info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
        image_info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (!(desc->Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;

    /* Additional usage flags for shader-based copies */
    typeless_format = vkd3d_get_typeless_format(device, format->dxgi_format);

    if (typeless_format == DXGI_FORMAT_R32_TYPELESS ||
            typeless_format == DXGI_FORMAT_R16_TYPELESS ||
            typeless_format == DXGI_FORMAT_R8_TYPELESS)
    {
        image_info.usage |= (format->vk_aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT)
                ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    if (vkd3d_resource_can_be_vrs(device, heap_properties, desc))
        image_info.usage |= VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;

    use_concurrent = !!(device->unique_queue_mask & (device->unique_queue_mask - 1));

    if (!(desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
    {
        /* Ignore config flags for actual simultaneous access cases. */
        if (((desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
                (vkd3d_config_flags & VKD3D_CONFIG_FLAG_FORCE_RTV_EXCLUSIVE_QUEUE)) ||
                ((desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
                 (vkd3d_config_flags & VKD3D_CONFIG_FLAG_FORCE_DSV_EXCLUSIVE_QUEUE)))
        {
            use_concurrent = false;
        }
    }

    if (use_concurrent)
    {
        /* For multi-queue, we have to use CONCURRENT since D3D does
         * not give us enough information to do ownership transfers. */
        image_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        image_info.queueFamilyIndexCount = device->queue_family_count;
        image_info.pQueueFamilyIndices = device->queue_family_indices;
    }
    else
    {
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.queueFamilyIndexCount = 0;
        image_info.pQueueFamilyIndices = NULL;
    }

    if (heap_properties && is_cpu_accessible_heap(heap_properties))
    {
        image_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

        if ((vkd3d_config_flags & VKD3D_CONFIG_FLAG_IGNORE_RTV_HOST_VISIBLE) &&
                (image_info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        {
            WARN("Workaround applied. Ignoring RTV on linear resources.\n");
            image_info.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (resource)
                resource->desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }

        if (vkd3d_is_linear_tiling_supported(device, &image_info))
        {
            /* Required for ReadFromSubresource(). */
            WARN("Forcing VK_IMAGE_TILING_LINEAR for CPU readable texture.\n");
            image_info.tiling = VK_IMAGE_TILING_LINEAR;
        }
    }
    else
    {
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if (sparse_resource)
    {
        VkSparseImageFormatProperties sparse_infos[2];
        uint32_t sparse_info_count = ARRAY_SIZE(sparse_infos);

        // D3D12 only allows sparse images with one aspect, so we can only
        // get one struct for metadata aspect and one for the data aspect
        VK_CALL(vkGetPhysicalDeviceSparseImageFormatProperties(
                device->vk_physical_device, image_info.format,
                image_info.imageType, image_info.samples, image_info.usage,
                image_info.tiling, &sparse_info_count, sparse_infos));

        if (!sparse_info_count)
        {
            ERR("Sparse images not supported with format %u, type %u, samples %u, usage %#x, tiling %u.\n",
                    image_info.format, image_info.imageType, image_info.samples, image_info.usage, image_info.tiling);
            return E_INVALIDARG;
        }

        for (i = 0; i < sparse_info_count; i++)
        {
            if (sparse_infos[i].aspectMask & VK_IMAGE_ASPECT_METADATA_BIT)
                continue;

            if (vkd3d_sparse_image_may_have_mip_tail(desc, &sparse_infos[i]) && desc->DepthOrArraySize > 1 && desc->MipLevels > 1)
            {
                WARN("Multiple array layers not supported for sparse images with mip tail.\n");
                return E_INVALIDARG;
            }
        }
    }

    if (resource)
    {
        if (image_info.tiling == VK_IMAGE_TILING_LINEAR)
        {
            resource->flags |= VKD3D_RESOURCE_LINEAR_TILING;
            resource->common_layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        else
            resource->common_layout = vk_common_image_layout_from_d3d12_desc(desc);

        if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
            resource->flags |= VKD3D_RESOURCE_SIMULTANEOUS_ACCESS;
    }

    if ((vr = VK_CALL(vkCreateImage(device->vk_device, &image_info, NULL, vk_image))) < 0)
        WARN("Failed to create Vulkan image, vr %d.\n", vr);

    return hresult_from_vk_result(vr);
}

HRESULT vkd3d_get_image_allocation_info(struct d3d12_device *device,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_ALLOCATION_INFO *allocation_info)
{
    static const D3D12_HEAP_PROPERTIES heap_properties = {D3D12_HEAP_TYPE_DEFAULT};
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    D3D12_RESOURCE_DESC validated_desc;
    VkMemoryRequirements requirements;
    VkDeviceSize target_alignment;
    VkImage vk_image;
    HRESULT hr;

    assert(desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER);
    assert(d3d12_resource_validate_desc(desc, device) == S_OK);

    if (!desc->MipLevels)
    {
        validated_desc = *desc;
        validated_desc.MipLevels = max_miplevel_count(desc);
        desc = &validated_desc;
    }

    /* XXX: We have to create an image to get its memory requirements. */
    if (FAILED(hr = vkd3d_create_image(device, &heap_properties, 0, desc, NULL, &vk_image)))
        return hr;

    VK_CALL(vkGetImageMemoryRequirements(device->vk_device, vk_image, &requirements));
    VK_CALL(vkDestroyImage(device->vk_device, vk_image, NULL));

    allocation_info->SizeInBytes = requirements.size;
    allocation_info->Alignment = requirements.alignment;

    /* Do not report alignments greater than DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT
     * since that might confuse apps. Instead, pad the allocation so that we can
     * align the image ourselves. */
    target_alignment = desc->Alignment ? desc->Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    if (allocation_info->Alignment > target_alignment)
    {
        allocation_info->SizeInBytes += allocation_info->Alignment - target_alignment;
        allocation_info->Alignment = target_alignment;
    }

    return hr;
}

struct vkd3d_view_entry
{
    struct hash_map_entry entry;
    struct vkd3d_view_key key;
    struct vkd3d_view *view;
};

static bool d3d12_sampler_needs_border_color(D3D12_TEXTURE_ADDRESS_MODE u,
        D3D12_TEXTURE_ADDRESS_MODE v, D3D12_TEXTURE_ADDRESS_MODE w);

static uint32_t vkd3d_view_entry_hash(const void *key)
{
    const struct vkd3d_view_key *k = key;
    uint32_t hash;

    switch (k->view_type)
    {
        case VKD3D_VIEW_TYPE_BUFFER:
        case VKD3D_VIEW_TYPE_ACCELERATION_STRUCTURE:
            hash = hash_uint64((uint64_t)k->u.buffer.buffer);
            hash = hash_combine(hash, hash_uint64(k->u.buffer.offset));
            hash = hash_combine(hash, hash_uint64(k->u.buffer.size));
            hash = hash_combine(hash, (uintptr_t)k->u.buffer.format);
            break;

        case VKD3D_VIEW_TYPE_IMAGE:
            hash = hash_uint64((uint64_t)k->u.texture.image);
            hash = hash_combine(hash, k->u.texture.view_type);
            hash = hash_combine(hash, (uintptr_t)k->u.texture.format);
            hash = hash_combine(hash, k->u.texture.miplevel_idx);
            hash = hash_combine(hash, k->u.texture.miplevel_count);
            hash = hash_combine(hash, float_bits_to_uint32(k->u.texture.miplevel_clamp));
            hash = hash_combine(hash, k->u.texture.layer_idx);
            hash = hash_combine(hash, k->u.texture.layer_count);
            hash = hash_combine(hash, k->u.texture.components.r);
            hash = hash_combine(hash, k->u.texture.components.g);
            hash = hash_combine(hash, k->u.texture.components.b);
            hash = hash_combine(hash, k->u.texture.components.a);
            hash = hash_combine(hash, k->u.texture.allowed_swizzle);
            break;

        case VKD3D_VIEW_TYPE_SAMPLER:
            hash = (uint32_t)k->u.sampler.Filter;
            hash = hash_combine(hash, (uint32_t)k->u.sampler.AddressU);
            hash = hash_combine(hash, (uint32_t)k->u.sampler.AddressV);
            hash = hash_combine(hash, (uint32_t)k->u.sampler.AddressW);
            hash = hash_combine(hash, float_bits_to_uint32(k->u.sampler.MipLODBias));
            hash = hash_combine(hash, (uint32_t)k->u.sampler.MaxAnisotropy);
            hash = hash_combine(hash, (uint32_t)k->u.sampler.ComparisonFunc);
            if (d3d12_sampler_needs_border_color(k->u.sampler.AddressU, k->u.sampler.AddressV, k->u.sampler.AddressW))
            {
                hash = hash_combine(hash, float_bits_to_uint32(k->u.sampler.BorderColor[0]));
                hash = hash_combine(hash, float_bits_to_uint32(k->u.sampler.BorderColor[1]));
                hash = hash_combine(hash, float_bits_to_uint32(k->u.sampler.BorderColor[2]));
                hash = hash_combine(hash, float_bits_to_uint32(k->u.sampler.BorderColor[3]));
            }
            hash = hash_combine(hash, float_bits_to_uint32(k->u.sampler.MinLOD));
            hash = hash_combine(hash, float_bits_to_uint32(k->u.sampler.MaxLOD));
            break;

        default:
            ERR("Unexpected view type %d.\n", k->view_type);
            return 0;
    }

    return hash;
}

static bool vkd3d_view_entry_compare(const void *key, const struct hash_map_entry *entry)
{
    const struct vkd3d_view_entry *e = (const struct vkd3d_view_entry*) entry;
    const struct vkd3d_view_key *k = key;

    if (k->view_type != e->key.view_type)
        return false;

    switch (k->view_type)
    {
        case VKD3D_VIEW_TYPE_BUFFER:
        case VKD3D_VIEW_TYPE_ACCELERATION_STRUCTURE:
            return k->u.buffer.buffer == e->key.u.buffer.buffer &&
                    k->u.buffer.format == e->key.u.buffer.format &&
                    k->u.buffer.offset == e->key.u.buffer.offset &&
                    k->u.buffer.size == e->key.u.buffer.size;

        case VKD3D_VIEW_TYPE_IMAGE:
            return k->u.texture.image == e->key.u.texture.image &&
                    k->u.texture.view_type == e->key.u.texture.view_type &&
                    k->u.texture.format == e->key.u.texture.format &&
                    k->u.texture.miplevel_idx == e->key.u.texture.miplevel_idx &&
                    k->u.texture.miplevel_count == e->key.u.texture.miplevel_count &&
                    k->u.texture.miplevel_clamp == e->key.u.texture.miplevel_clamp &&
                    k->u.texture.layer_idx == e->key.u.texture.layer_idx &&
                    k->u.texture.layer_count == e->key.u.texture.layer_count &&
                    k->u.texture.components.r == e->key.u.texture.components.r &&
                    k->u.texture.components.g == e->key.u.texture.components.g &&
                    k->u.texture.components.b == e->key.u.texture.components.b &&
                    k->u.texture.components.a == e->key.u.texture.components.a &&
                    k->u.texture.allowed_swizzle == e->key.u.texture.allowed_swizzle;

        case VKD3D_VIEW_TYPE_SAMPLER:
            return k->u.sampler.Filter == e->key.u.sampler.Filter &&
                    k->u.sampler.AddressU == e->key.u.sampler.AddressU &&
                    k->u.sampler.AddressV == e->key.u.sampler.AddressV &&
                    k->u.sampler.AddressW == e->key.u.sampler.AddressW &&
                    k->u.sampler.MipLODBias == e->key.u.sampler.MipLODBias &&
                    k->u.sampler.MaxAnisotropy == e->key.u.sampler.MaxAnisotropy &&
                    k->u.sampler.ComparisonFunc == e->key.u.sampler.ComparisonFunc &&
                    (!d3d12_sampler_needs_border_color(k->u.sampler.AddressU, k->u.sampler.AddressV, k->u.sampler.AddressW) ||
                        (k->u.sampler.BorderColor[0] == e->key.u.sampler.BorderColor[0] &&
                        k->u.sampler.BorderColor[1] == e->key.u.sampler.BorderColor[1] &&
                        k->u.sampler.BorderColor[2] == e->key.u.sampler.BorderColor[2] &&
                        k->u.sampler.BorderColor[3] == e->key.u.sampler.BorderColor[3])) &&
                    k->u.sampler.MinLOD == e->key.u.sampler.MinLOD &&
                    k->u.sampler.MaxLOD == e->key.u.sampler.MaxLOD;
            break;

        default:
            ERR("Unexpected view type %d.\n", k->view_type);
            return false;
    }
}

HRESULT vkd3d_view_map_init(struct vkd3d_view_map *view_map)
{
    view_map->spinlock = 0;
    hash_map_init(&view_map->map, &vkd3d_view_entry_hash, &vkd3d_view_entry_compare, sizeof(struct vkd3d_view_entry));
    return S_OK;
}

static void vkd3d_view_destroy(struct vkd3d_view *view, struct d3d12_device *device);

void vkd3d_view_map_destroy(struct vkd3d_view_map *view_map, struct d3d12_device *device)
{
    uint32_t i;

    for (i = 0; i < view_map->map.entry_count; i++)
    {
        struct vkd3d_view_entry *e = (struct vkd3d_view_entry *)hash_map_get_entry(&view_map->map, i);

        if (e->entry.flags & HASH_MAP_ENTRY_OCCUPIED)
            vkd3d_view_destroy(e->view, device);
    }

    hash_map_clear(&view_map->map);
}

static struct vkd3d_view *vkd3d_view_create(enum vkd3d_view_type type);

static HRESULT d3d12_create_sampler(struct d3d12_device *device,
        const D3D12_SAMPLER_DESC *desc, VkSampler *vk_sampler);

struct vkd3d_view *vkd3d_view_map_create_view(struct vkd3d_view_map *view_map,
        struct d3d12_device *device, const struct vkd3d_view_key *key)
{
    struct vkd3d_view_entry entry, *e;
    struct vkd3d_view *redundant_view;
    struct vkd3d_view *view;
    bool success;

    /* In the steady state, we will be reading existing entries from a view map.
     * Prefer read-write spinlocks here to reduce contention as much as possible. */
    rw_spinlock_acquire_read(&view_map->spinlock);

    if ((e = (struct vkd3d_view_entry *)hash_map_find(&view_map->map, key)))
    {
        view = e->view;
        rw_spinlock_release_read(&view_map->spinlock);
        return view;
    }

    rw_spinlock_release_read(&view_map->spinlock);

    switch (key->view_type)
    {
        case VKD3D_VIEW_TYPE_BUFFER:
            success = vkd3d_create_buffer_view(device, &key->u.buffer, &view);
            break;

        case VKD3D_VIEW_TYPE_IMAGE:
            success = vkd3d_create_texture_view(device, &key->u.texture, &view);
            break;

        case VKD3D_VIEW_TYPE_SAMPLER:
            success = (view = vkd3d_view_create(VKD3D_VIEW_TYPE_SAMPLER)) &&
                    SUCCEEDED(d3d12_create_sampler(device, &key->u.sampler, &view->vk_sampler));
            break;

        case VKD3D_VIEW_TYPE_ACCELERATION_STRUCTURE:
            success = vkd3d_create_acceleration_structure_view(device, &key->u.buffer, &view);
            break;

        default:
            ERR("Unsupported view type %u.\n", key->view_type);
            success = false;
            break;
    }

    if (!success)
        return NULL;

    vkd3d_descriptor_debug_register_view_cookie(device->descriptor_qa_global_info,
            view->cookie, view_map->resource_cookie);

    entry.key = *key;
    entry.view = view;

    rw_spinlock_acquire_write(&view_map->spinlock);

    if (!(e = (struct vkd3d_view_entry *)hash_map_insert(&view_map->map, key, &entry.entry)))
        ERR("Failed to insert view into hash map.\n");

    if (e->view != view)
    {
        /* We yielded on the insert because another thread came in-between, and allocated a new hash map entry.
         * This can happen between releasing reader lock, and acquiring writer lock. */
        redundant_view = view;
        view = e->view;
        rw_spinlock_release_write(&view_map->spinlock);
        vkd3d_view_decref(redundant_view, device);
    }
    else
    {
        /* If we start emitting too many typed SRVs, we will eventually crash on NV, since
         * VkBufferView objects appear to consume GPU resources. */
        if ((view_map->map.used_count % 1024) == 0)
            ERR("Intense view map pressure! Got %u views in hash map %p.\n", view_map->map.used_count, &view_map->map);

        view = e->view;
        rw_spinlock_release_write(&view_map->spinlock);
    }

    return view;
}

struct vkd3d_sampler_key
{
    D3D12_STATIC_SAMPLER_DESC desc;
};

struct vkd3d_sampler_entry
{
    struct hash_map_entry entry;
    D3D12_STATIC_SAMPLER_DESC desc;
    VkSampler vk_sampler;
};

static uint32_t vkd3d_sampler_entry_hash(const void *key)
{
    const struct vkd3d_sampler_key *k = key;
    uint32_t hash;

    hash = (uint32_t)k->desc.Filter;
    hash = hash_combine(hash, (uint32_t)k->desc.AddressU);
    hash = hash_combine(hash, (uint32_t)k->desc.AddressV);
    hash = hash_combine(hash, (uint32_t)k->desc.AddressW);
    hash = hash_combine(hash, float_bits_to_uint32(k->desc.MipLODBias));
    hash = hash_combine(hash, k->desc.MaxAnisotropy);
    hash = hash_combine(hash, (uint32_t)k->desc.ComparisonFunc);
    hash = hash_combine(hash, (uint32_t)k->desc.BorderColor);
    hash = hash_combine(hash, float_bits_to_uint32(k->desc.MinLOD));
    hash = hash_combine(hash, float_bits_to_uint32(k->desc.MaxLOD));
    return hash;
}

static bool vkd3d_sampler_entry_compare(const void *key, const struct hash_map_entry *entry)
{
    const struct vkd3d_sampler_entry *e = (const struct vkd3d_sampler_entry*) entry;
    const struct vkd3d_sampler_key *k = key;

    return k->desc.Filter == e->desc.Filter &&
            k->desc.AddressU == e->desc.AddressU &&
            k->desc.AddressV == e->desc.AddressV &&
            k->desc.AddressW == e->desc.AddressW &&
            k->desc.MipLODBias == e->desc.MipLODBias &&
            k->desc.MaxAnisotropy == e->desc.MaxAnisotropy &&
            k->desc.ComparisonFunc == e->desc.ComparisonFunc &&
            k->desc.BorderColor == e->desc.BorderColor &&
            k->desc.MinLOD == e->desc.MinLOD &&
            k->desc.MaxLOD == e->desc.MaxLOD;
}

HRESULT vkd3d_sampler_state_init(struct vkd3d_sampler_state *state,
        struct d3d12_device *device)
{
    int rc;

    memset(state, 0, sizeof(*state));

    if ((rc = pthread_mutex_init(&state->mutex, NULL)))
        return hresult_from_errno(rc);

    hash_map_init(&state->map, &vkd3d_sampler_entry_hash, &vkd3d_sampler_entry_compare, sizeof(struct vkd3d_sampler_entry));
    return S_OK;
}

void vkd3d_sampler_state_cleanup(struct vkd3d_sampler_state *state,
        struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    uint32_t i;

    for (i = 0; i < state->vk_descriptor_pool_count; i++)
        VK_CALL(vkDestroyDescriptorPool(device->vk_device, state->vk_descriptor_pools[i], NULL));

    vkd3d_free(state->vk_descriptor_pools);

    for (i = 0; i < state->map.entry_count; i++)
    {
        struct vkd3d_sampler_entry *e = (struct vkd3d_sampler_entry *)hash_map_get_entry(&state->map, i);

        if (e->entry.flags & HASH_MAP_ENTRY_OCCUPIED)
            VK_CALL(vkDestroySampler(device->vk_device, e->vk_sampler, NULL));
    }

    hash_map_clear(&state->map);

    pthread_mutex_destroy(&state->mutex);
}

HRESULT d3d12_create_static_sampler(struct d3d12_device *device,
        const D3D12_STATIC_SAMPLER_DESC *desc, VkSampler *vk_sampler);

HRESULT vkd3d_sampler_state_create_static_sampler(struct vkd3d_sampler_state *state,
        struct d3d12_device *device, const D3D12_STATIC_SAMPLER_DESC *desc, VkSampler *vk_sampler)
{
    struct vkd3d_sampler_entry entry, *e;
    HRESULT hr;
    int rc;

    if ((rc = pthread_mutex_lock(&state->mutex)))
    {
        ERR("Failed to lock mutex, rc %d.\n", rc);
        return hresult_from_errno(rc);
    }

    if ((e = (struct vkd3d_sampler_entry*)hash_map_find(&state->map, desc)))
    {
        *vk_sampler = e->vk_sampler;
        pthread_mutex_unlock(&state->mutex);
        return S_OK;
    }

    if (FAILED(hr = d3d12_create_static_sampler(device, desc, vk_sampler)))
    {
        pthread_mutex_unlock(&state->mutex);
        return hr;
    }

    entry.desc = *desc;
    entry.vk_sampler = *vk_sampler;

    if (!hash_map_insert(&state->map, desc, &entry.entry))
        ERR("Failed to insert sampler into hash map.\n");

    pthread_mutex_unlock(&state->mutex);
    return S_OK;
}

static VkResult vkd3d_sampler_state_create_descriptor_pool(struct d3d12_device *device, VkDescriptorPool *vk_pool)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkDescriptorPoolCreateInfo pool_info;
    VkDescriptorPoolSize pool_size;

    pool_size.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_size.descriptorCount = 16384;

    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.pNext = NULL;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 4096;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    return VK_CALL(vkCreateDescriptorPool(device->vk_device, &pool_info, NULL, vk_pool));
}

HRESULT vkd3d_sampler_state_allocate_descriptor_set(struct vkd3d_sampler_state *state,
        struct d3d12_device *device, VkDescriptorSetLayout vk_layout, VkDescriptorSet *vk_set,
        VkDescriptorPool *vk_pool)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkResult vr = VK_ERROR_OUT_OF_POOL_MEMORY;
    VkDescriptorSetAllocateInfo alloc_info;
    size_t i;
    int rc;

    if ((rc = pthread_mutex_lock(&state->mutex)))
    {
        ERR("Failed to lock mutex, rc %d.\n", rc);
        return hresult_from_errno(rc);
    }

    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.pNext = NULL;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &vk_layout;

    for (i = 0; i < state->vk_descriptor_pool_count; i++)
    {
        alloc_info.descriptorPool = state->vk_descriptor_pools[i];
        vr = VK_CALL(vkAllocateDescriptorSets(device->vk_device, &alloc_info, vk_set));

        if (vr == VK_SUCCESS)
        {
            *vk_pool = alloc_info.descriptorPool;
            break;
        }
    }

    if (vr == VK_ERROR_OUT_OF_POOL_MEMORY || vr == VK_ERROR_FRAGMENTED_POOL)
    {
        vr = vkd3d_sampler_state_create_descriptor_pool(device, &alloc_info.descriptorPool);

        if (vr != VK_SUCCESS)
        {
            pthread_mutex_unlock(&state->mutex);
            return hresult_from_vk_result(vr);
        }

        if (!vkd3d_array_reserve((void **)&state->vk_descriptor_pools, &state->vk_descriptor_pools_size,
                state->vk_descriptor_pool_count + 1, sizeof(*state->vk_descriptor_pools)))
        {
            VK_CALL(vkDestroyDescriptorPool(device->vk_device, alloc_info.descriptorPool, NULL));
            pthread_mutex_unlock(&state->mutex);
            return E_OUTOFMEMORY;
        }

        state->vk_descriptor_pools[state->vk_descriptor_pool_count++] = alloc_info.descriptorPool;
        vr = VK_CALL(vkAllocateDescriptorSets(device->vk_device, &alloc_info, vk_set));
        *vk_pool = alloc_info.descriptorPool;
    }

    pthread_mutex_unlock(&state->mutex);
    return hresult_from_vk_result(vr);
}

void vkd3d_sampler_state_free_descriptor_set(struct vkd3d_sampler_state *state,
        struct d3d12_device *device, VkDescriptorSet vk_set, VkDescriptorPool vk_pool)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    int rc;

    if ((rc = pthread_mutex_lock(&state->mutex)))
        ERR("Failed to lock mutex, rc %d.\n", rc);

    if (vk_pool && vk_set)
        VK_CALL(vkFreeDescriptorSets(device->vk_device, vk_pool, 1, &vk_set));
    pthread_mutex_unlock(&state->mutex);
}

static void d3d12_resource_get_tiling(struct d3d12_device *device, struct d3d12_resource *resource,
        UINT *total_tile_count, D3D12_PACKED_MIP_INFO *packed_mip_info, D3D12_TILE_SHAPE *tile_shape,
        D3D12_SUBRESOURCE_TILING *tilings, VkSparseImageMemoryRequirements *vk_info)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkSparseImageMemoryRequirements *memory_requirements = NULL;
    unsigned int i, tile_count, packed_tiles, standard_mips;
    const D3D12_RESOURCE_DESC *desc = &resource->desc;
    uint32_t memory_requirement_count = 0;
    VkExtent3D block_extent;

    memset(vk_info, 0, sizeof(*vk_info));

    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        tile_count = align(desc->Width, VKD3D_TILE_SIZE) / VKD3D_TILE_SIZE;

        packed_mip_info->NumStandardMips = 0;
        packed_mip_info->NumPackedMips = 0;
        packed_mip_info->NumTilesForPackedMips = 0;
        packed_mip_info->StartTileIndexInOverallResource = 0;

        tile_shape->WidthInTexels = VKD3D_TILE_SIZE;
        tile_shape->HeightInTexels = 1;
        tile_shape->DepthInTexels = 1;

        tilings[0].WidthInTiles = tile_count;
        tilings[0].HeightInTiles = 1;
        tilings[0].DepthInTiles = 1;
        tilings[0].StartTileIndexInOverallResource = 0;

        *total_tile_count = tile_count;
    }
    else
    {
        VK_CALL(vkGetImageSparseMemoryRequirements(device->vk_device,
                resource->res.vk_image, &memory_requirement_count, NULL));

        if (!memory_requirement_count)
        {
            ERR("Failed to query sparse memory requirements.\n");
            return;
        }

        memory_requirements = vkd3d_malloc(memory_requirement_count * sizeof(*memory_requirements));

        VK_CALL(vkGetImageSparseMemoryRequirements(device->vk_device,
                resource->res.vk_image, &memory_requirement_count, memory_requirements));

        for (i = 0; i < memory_requirement_count; i++)
        {
            if (!(memory_requirements[i].formatProperties.aspectMask & VK_IMAGE_ASPECT_METADATA_BIT))
                *vk_info = memory_requirements[i];
        }

        vkd3d_free(memory_requirements);

        /* Assume that there is no mip tail if either the size is zero or
         * if the first LOD is out of range. It's not clear what drivers
         * are supposed to report here if the image has no mip tail. */
        standard_mips = vk_info->imageMipTailSize
                ? min(desc->MipLevels, vk_info->imageMipTailFirstLod)
                : desc->MipLevels;

        packed_tiles = standard_mips < desc->MipLevels
                ? align(vk_info->imageMipTailSize, VKD3D_TILE_SIZE) / VKD3D_TILE_SIZE
                : 0;

        if (!(vk_info->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT))
            packed_tiles *= d3d12_resource_desc_get_layer_count(desc);

        block_extent = vk_info->formatProperties.imageGranularity;
        tile_count = 0;

        for (i = 0; i < d3d12_resource_desc_get_sub_resource_count_per_plane(desc); i++)
        {
            unsigned int mip_level = i % desc->MipLevels;
            unsigned int tile_count_w = align(d3d12_resource_desc_get_width(desc, mip_level), block_extent.width) / block_extent.width;
            unsigned int tile_count_h = align(d3d12_resource_desc_get_height(desc, mip_level), block_extent.height) / block_extent.height;
            unsigned int tile_count_d = align(d3d12_resource_desc_get_depth(desc, mip_level), block_extent.depth) / block_extent.depth;

            if (mip_level < standard_mips)
            {
                tilings[i].WidthInTiles = tile_count_w;
                tilings[i].HeightInTiles = tile_count_h;
                tilings[i].DepthInTiles = tile_count_d;
                tilings[i].StartTileIndexInOverallResource = tile_count;
                tile_count += tile_count_w * tile_count_h * tile_count_d;
            }
            else
            {
                tilings[i].WidthInTiles = 0;
                tilings[i].HeightInTiles = 0;
                tilings[i].DepthInTiles = 0;
                tilings[i].StartTileIndexInOverallResource = ~0u;
            }
        }

        packed_mip_info->NumStandardMips = standard_mips;
        packed_mip_info->NumTilesForPackedMips = packed_tiles;
        packed_mip_info->NumPackedMips = desc->MipLevels - standard_mips;
        packed_mip_info->StartTileIndexInOverallResource = packed_tiles ? tile_count : 0;

        tile_count += packed_tiles;

        if (standard_mips)
        {
            tile_shape->WidthInTexels = block_extent.width;
            tile_shape->HeightInTexels = block_extent.height;
            tile_shape->DepthInTexels = block_extent.depth;
        }
        else
        {
            tile_shape->WidthInTexels = 0;
            tile_shape->HeightInTexels = 0;
            tile_shape->DepthInTexels = 0;
        }

        *total_tile_count = tile_count;
    }
}

static void d3d12_resource_destroy(struct d3d12_resource *resource, struct d3d12_device *device);

static ULONG d3d12_resource_incref(struct d3d12_resource *resource)
{
    ULONG refcount = InterlockedIncrement(&resource->internal_refcount);

    TRACE("%p increasing refcount to %u.\n", resource, refcount);

    return refcount;
}

static ULONG d3d12_resource_decref(struct d3d12_resource *resource)
{
    ULONG refcount = InterlockedDecrement(&resource->internal_refcount);

    TRACE("%p decreasing refcount to %u.\n", resource, refcount);

    if (!refcount)
        d3d12_resource_destroy(resource, resource->device);

    return refcount;
}

bool d3d12_resource_is_cpu_accessible(const struct d3d12_resource *resource)
{
    return !(resource->flags & VKD3D_RESOURCE_RESERVED) &&
            is_cpu_accessible_heap(&resource->heap_properties);
}

static bool d3d12_resource_validate_box(const struct d3d12_resource *resource,
        unsigned int sub_resource_idx, const D3D12_BOX *box)
{
    unsigned int mip_level = sub_resource_idx % resource->desc.MipLevels;
    uint32_t width_mask, height_mask;
    uint64_t width, height, depth;

    width = d3d12_resource_desc_get_width(&resource->desc, mip_level);
    height = d3d12_resource_desc_get_height(&resource->desc, mip_level);
    depth = d3d12_resource_desc_get_depth(&resource->desc, mip_level);

    width_mask = resource->format->block_width - 1;
    height_mask = resource->format->block_height - 1;

    return box->left <= width && box->right <= width
            && box->top <= height && box->bottom <= height
            && box->front <= depth && box->back <= depth
            && !(box->left & width_mask)
            && !(box->right & width_mask)
            && !(box->top & height_mask)
            && !(box->bottom & height_mask);
}

static void d3d12_resource_get_level_box(const struct d3d12_resource *resource,
        unsigned int level, D3D12_BOX *box)
{
    box->left = 0;
    box->top = 0;
    box->front = 0;
    box->right = d3d12_resource_desc_get_width(&resource->desc, level);
    box->bottom = d3d12_resource_desc_get_height(&resource->desc, level);
    box->back = d3d12_resource_desc_get_depth(&resource->desc, level);
}

static void d3d12_resource_set_name(struct d3d12_resource *resource, const char *name)
{
    /* Multiple committed and placed buffers may refer to the same VkBuffer,
     * which may cause race conditions if the app calls this concurrently */
    if (d3d12_resource_is_buffer(resource) && (resource->flags & VKD3D_RESOURCE_RESERVED))
        vkd3d_set_vk_object_name(resource->device, (uint64_t)resource->res.vk_buffer,
                VK_OBJECT_TYPE_BUFFER, name);
    else if (d3d12_resource_is_texture(resource))
        vkd3d_set_vk_object_name(resource->device, (uint64_t)resource->res.vk_image,
                VK_OBJECT_TYPE_IMAGE, name);
}

/* ID3D12Resource */
static HRESULT STDMETHODCALLTYPE d3d12_resource_QueryInterface(d3d12_resource_iface *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D12Resource)
            || IsEqualGUID(riid, &IID_ID3D12Resource1)
            || IsEqualGUID(riid, &IID_ID3D12Pageable)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12Resource_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_resource_AddRef(d3d12_resource_iface *iface)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);
    ULONG refcount = InterlockedIncrement(&resource->refcount);

    TRACE("%p increasing refcount to %u.\n", resource, refcount);

    if (refcount == 1)
    {
        struct d3d12_device *device = resource->device;

        d3d12_device_add_ref(device);
        d3d12_resource_incref(resource);
    }

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_resource_Release(d3d12_resource_iface *iface)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);
    ULONG refcount = InterlockedDecrement(&resource->refcount);

    TRACE("%p decreasing refcount to %u.\n", resource, refcount);

    if (!refcount)
        d3d12_resource_decref(resource);

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_GetPrivateData(d3d12_resource_iface *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&resource->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_SetPrivateData(d3d12_resource_iface *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&resource->private_store, guid, data_size, data,
            (vkd3d_set_name_callback) d3d12_resource_set_name, resource);
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_SetPrivateDataInterface(d3d12_resource_iface *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&resource->private_store, guid, data,
            (vkd3d_set_name_callback) d3d12_resource_set_name, resource);
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_GetDevice(d3d12_resource_iface *iface, REFIID iid, void **device)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);

    TRACE("iface %p, iid %s, device %p.\n", iface, debugstr_guid(iid), device);

    return d3d12_device_query_interface(resource->device, iid, device);
}

static bool d3d12_resource_get_mapped_memory_range(struct d3d12_resource *resource,
        UINT subresource, const D3D12_RANGE *range, VkMappedMemoryRange *vk_mapped_range)
{
    const struct d3d12_device *device = resource->device;

    if (range && range->End <= range->Begin)
        return false;

    if (device->memory_properties.memoryTypes[resource->mem.device_allocation.vk_memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        return false;

    vk_mapped_range->sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    vk_mapped_range->pNext = NULL;
    vk_mapped_range->memory = resource->mem.device_allocation.vk_memory;

    if (resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        vk_mapped_range->offset = resource->mem.offset;
        vk_mapped_range->size = resource->desc.Width;
    }
    else
    {
        FIXME("Not implemented for textures.\n");
        return false;
    }

    if (range)
    {
        vk_mapped_range->offset += range->Begin;
        vk_mapped_range->size = range->End - range->Begin;
    }

    return true;
}

static void d3d12_resource_invalidate_range(struct d3d12_resource *resource,
        UINT subresource, const D3D12_RANGE *read_range)
{
    const struct vkd3d_vk_device_procs *vk_procs = &resource->device->vk_procs;
    VkMappedMemoryRange mapped_range;

    if (!d3d12_resource_get_mapped_memory_range(resource, subresource, read_range, &mapped_range))
        return;

    VK_CALL(vkInvalidateMappedMemoryRanges(resource->device->vk_device, 1, &mapped_range));
}

static void d3d12_resource_flush_range(struct d3d12_resource *resource,
        UINT subresource, const D3D12_RANGE *written_range)
{
    const struct vkd3d_vk_device_procs *vk_procs = &resource->device->vk_procs;
    VkMappedMemoryRange mapped_range;

    if (!d3d12_resource_get_mapped_memory_range(resource, subresource, written_range, &mapped_range))
        return;

    VK_CALL(vkFlushMappedMemoryRanges(resource->device->vk_device, 1, &mapped_range));
}

static void d3d12_resource_get_map_ptr(struct d3d12_resource *resource, void **data)
{
    assert(resource->mem.cpu_address);
    *data = resource->mem.cpu_address;
}

static bool d3d12_resource_texture_validate_map(struct d3d12_resource *resource)
{
    bool invalid_map;
    /* Very special case that is explicitly called out in the D3D12 validation layers. */
    invalid_map = resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
            resource->desc.MipLevels > 1;
    return !invalid_map;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_Map(d3d12_resource_iface *iface, UINT sub_resource,
        const D3D12_RANGE *read_range, void **data)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);
    unsigned int sub_resource_count;

    TRACE("iface %p, sub_resource %u, read_range %p, data %p.\n",
            iface, sub_resource, read_range, data);

    if (!d3d12_resource_is_cpu_accessible(resource))
    {
        WARN("Resource is not CPU accessible.\n");
        return E_INVALIDARG;
    }

    sub_resource_count = d3d12_resource_get_sub_resource_count(resource);
    if (sub_resource >= sub_resource_count)
    {
        WARN("Sub-resource index %u is out of range (%u sub-resources).\n", sub_resource, sub_resource_count);
        return E_INVALIDARG;
    }

    if (d3d12_resource_is_texture(resource) && (data || !d3d12_resource_texture_validate_map(resource)))
    {
        /* Cannot get pointer to mapped texture.
         * It is only possible to make UNKNOWN textures host visible,
         * and only NULL map + Write/ReadSubresource is allowed in this scenario. */
        return E_INVALIDARG;
    }

    if (resource->flags & VKD3D_RESOURCE_RESERVED)
    {
        FIXME("Not implemented for this resource type.\n");
        return E_NOTIMPL;
    }

    if (data)
    {
        d3d12_resource_get_map_ptr(resource, data);
        TRACE("Returning pointer %p.\n", *data);
    }

    d3d12_resource_invalidate_range(resource, sub_resource, read_range);
    return S_OK;
}

static void STDMETHODCALLTYPE d3d12_resource_Unmap(d3d12_resource_iface *iface, UINT sub_resource,
        const D3D12_RANGE *written_range)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);
    unsigned int sub_resource_count;

    TRACE("iface %p, sub_resource %u, written_range %p.\n",
            iface, sub_resource, written_range);

    sub_resource_count = d3d12_resource_get_sub_resource_count(resource);
    if (sub_resource >= sub_resource_count)
    {
        WARN("Sub-resource index %u is out of range (%u sub-resources).\n", sub_resource, sub_resource_count);
        return;
    }

    d3d12_resource_flush_range(resource, sub_resource, written_range);
}

static D3D12_RESOURCE_DESC * STDMETHODCALLTYPE d3d12_resource_GetDesc(d3d12_resource_iface *iface,
        D3D12_RESOURCE_DESC *resource_desc)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);

    TRACE("iface %p, resource_desc %p.\n", iface, resource_desc);

    *resource_desc = resource->desc;
    return resource_desc;
}

static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE d3d12_resource_GetGPUVirtualAddress(d3d12_resource_iface *iface)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);

    TRACE("iface %p.\n", iface);

    return resource->res.va;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_WriteToSubresource(d3d12_resource_iface *iface,
        UINT dst_sub_resource, const D3D12_BOX *dst_box, const void *src_data,
        UINT src_row_pitch, UINT src_slice_pitch)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    VkImageSubresource vk_sub_resource;
    VkSubresourceLayout vk_layout;
    struct d3d12_device *device;
    uint8_t *dst_data;
    D3D12_BOX box;

    TRACE("iface %p, src_data %p, src_row_pitch %u, src_slice_pitch %u, "
            "dst_sub_resource %u, dst_box %s.\n",
            iface, src_data, src_row_pitch, src_slice_pitch, dst_sub_resource, debug_d3d12_box(dst_box));

    if (d3d12_resource_is_buffer(resource))
    {
        WARN("Buffers are not supported.\n");
        return E_INVALIDARG;
    }

    device = resource->device;
    vk_procs = &device->vk_procs;

    if (resource->format->vk_aspect_mask != VK_IMAGE_ASPECT_COLOR_BIT)
    {
        FIXME("Not supported for format %#x.\n", resource->format->dxgi_format);
        return E_NOTIMPL;
    }

    vk_sub_resource.arrayLayer = dst_sub_resource / resource->desc.MipLevels;
    vk_sub_resource.mipLevel = dst_sub_resource % resource->desc.MipLevels;
    vk_sub_resource.aspectMask = resource->format->vk_aspect_mask;

    if (!dst_box)
    {
        d3d12_resource_get_level_box(resource, vk_sub_resource.mipLevel, &box);
        dst_box = &box;
    }
    else if (!d3d12_resource_validate_box(resource, dst_sub_resource, dst_box))
    {
        WARN("Invalid box %s.\n", debug_d3d12_box(dst_box));
        return E_INVALIDARG;
    }

    if (d3d12_box_is_empty(dst_box))
    {
        WARN("Empty box %s.\n", debug_d3d12_box(dst_box));
        return S_OK;
    }

    if (!d3d12_resource_is_cpu_accessible(resource))
    {
        FIXME_ONCE("Not implemented for this resource type.\n");
        return E_NOTIMPL;
    }
    if (!(resource->flags & VKD3D_RESOURCE_LINEAR_TILING))
    {
        FIXME_ONCE("Not implemented for image tiling other than VK_IMAGE_TILING_LINEAR.\n");
        return E_NOTIMPL;
    }

    VK_CALL(vkGetImageSubresourceLayout(device->vk_device, resource->res.vk_image, &vk_sub_resource, &vk_layout));
    TRACE("Offset %#"PRIx64", size %#"PRIx64", row pitch %#"PRIx64", depth pitch %#"PRIx64".\n",
            vk_layout.offset, vk_layout.size, vk_layout.rowPitch, vk_layout.depthPitch);

    d3d12_resource_get_map_ptr(resource, (void **)&dst_data);

    dst_data += vk_layout.offset + vkd3d_format_get_data_offset(resource->format, vk_layout.rowPitch,
            vk_layout.depthPitch, dst_box->left, dst_box->top, dst_box->front);

    vkd3d_format_copy_data(resource->format, src_data, src_row_pitch, src_slice_pitch,
            dst_data, vk_layout.rowPitch, vk_layout.depthPitch, dst_box->right - dst_box->left,
            dst_box->bottom - dst_box->top, dst_box->back - dst_box->front);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_ReadFromSubresource(d3d12_resource_iface *iface,
        void *dst_data, UINT dst_row_pitch, UINT dst_slice_pitch,
        UINT src_sub_resource, const D3D12_BOX *src_box)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);
    const struct vkd3d_vk_device_procs *vk_procs;
    VkImageSubresource vk_sub_resource;
    VkSubresourceLayout vk_layout;
    struct d3d12_device *device;
    uint8_t *src_data;
    D3D12_BOX box;

    TRACE("iface %p, dst_data %p, dst_row_pitch %u, dst_slice_pitch %u, "
            "src_sub_resource %u, src_box %s.\n",
            iface, dst_data, dst_row_pitch, dst_slice_pitch, src_sub_resource, debug_d3d12_box(src_box));

    if (d3d12_resource_is_buffer(resource))
    {
        WARN("Buffers are not supported.\n");
        return E_INVALIDARG;
    }

    device = resource->device;
    vk_procs = &device->vk_procs;

    if (resource->format->vk_aspect_mask != VK_IMAGE_ASPECT_COLOR_BIT)
    {
        FIXME("Not supported for format %#x.\n", resource->format->dxgi_format);
        return E_NOTIMPL;
    }

    vk_sub_resource.arrayLayer = src_sub_resource / resource->desc.MipLevels;
    vk_sub_resource.mipLevel = src_sub_resource % resource->desc.MipLevels;
    vk_sub_resource.aspectMask = resource->format->vk_aspect_mask;

    if (!src_box)
    {
        d3d12_resource_get_level_box(resource, vk_sub_resource.mipLevel, &box);
        src_box = &box;
    }
    else if (!d3d12_resource_validate_box(resource, src_sub_resource, src_box))
    {
        WARN("Invalid box %s.\n", debug_d3d12_box(src_box));
        return E_INVALIDARG;
    }

    if (d3d12_box_is_empty(src_box))
    {
        WARN("Empty box %s.\n", debug_d3d12_box(src_box));
        return S_OK;
    }

    if (!d3d12_resource_is_cpu_accessible(resource))
    {
        FIXME_ONCE("Not implemented for this resource type.\n");
        return E_NOTIMPL;
    }
    if (!(resource->flags & VKD3D_RESOURCE_LINEAR_TILING))
    {
        FIXME_ONCE("Not implemented for image tiling other than VK_IMAGE_TILING_LINEAR.\n");
        return E_NOTIMPL;
    }

    VK_CALL(vkGetImageSubresourceLayout(device->vk_device, resource->res.vk_image, &vk_sub_resource, &vk_layout));
    TRACE("Offset %#"PRIx64", size %#"PRIx64", row pitch %#"PRIx64", depth pitch %#"PRIx64".\n",
            vk_layout.offset, vk_layout.size, vk_layout.rowPitch, vk_layout.depthPitch);

    d3d12_resource_get_map_ptr(resource, (void **)&src_data);

    src_data += vk_layout.offset + vkd3d_format_get_data_offset(resource->format, vk_layout.rowPitch,
            vk_layout.depthPitch, src_box->left, src_box->top, src_box->front);

    vkd3d_format_copy_data(resource->format, src_data, vk_layout.rowPitch, vk_layout.depthPitch,
            dst_data, dst_row_pitch, dst_slice_pitch, src_box->right - src_box->left,
            src_box->bottom - src_box->top, src_box->back - src_box->front);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_GetHeapProperties(d3d12_resource_iface *iface,
        D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS *flags)
{
    struct d3d12_resource *resource = impl_from_ID3D12Resource1(iface);

    TRACE("iface %p, heap_properties %p, flags %p.\n",
            iface, heap_properties, flags);

    if (resource->flags & VKD3D_RESOURCE_EXTERNAL)
    {
        if (heap_properties)
        {
            memset(heap_properties, 0, sizeof(*heap_properties));
            heap_properties->Type = D3D12_HEAP_TYPE_DEFAULT;
            heap_properties->CreationNodeMask = 1;
            heap_properties->VisibleNodeMask = 1;
        }
        if (flags)
            *flags = D3D12_HEAP_FLAG_NONE;
        return S_OK;
    }

    if (resource->flags & VKD3D_RESOURCE_RESERVED)
    {
        WARN("Cannot get heap properties for reserved resources.\n");
        return E_INVALIDARG;
    }

    if (heap_properties)
        *heap_properties = resource->heap_properties;
    if (flags)
        *flags = resource->heap_flags;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE d3d12_resource_GetProtectedResourceSession(d3d12_resource_iface *iface,
        REFIID iid, void **protected_session)
{
    FIXME("iface %p, iid %s, protected_session %p stub!", iface, debugstr_guid(iid), protected_session);

    return E_NOTIMPL;
}

CONST_VTBL struct ID3D12Resource1Vtbl d3d12_resource_vtbl =
{
    /* IUnknown methods */
    d3d12_resource_QueryInterface,
    d3d12_resource_AddRef,
    d3d12_resource_Release,
    /* ID3D12Object methods */
    d3d12_resource_GetPrivateData,
    d3d12_resource_SetPrivateData,
    d3d12_resource_SetPrivateDataInterface,
    (void *)d3d12_object_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_resource_GetDevice,
    /* ID3D12Resource methods */
    d3d12_resource_Map,
    d3d12_resource_Unmap,
    d3d12_resource_GetDesc,
    d3d12_resource_GetGPUVirtualAddress,
    d3d12_resource_WriteToSubresource,
    d3d12_resource_ReadFromSubresource,
    d3d12_resource_GetHeapProperties,
    /* ID3D12Resource1 methods */
    d3d12_resource_GetProtectedResourceSession,
};

VkImageAspectFlags vk_image_aspect_flags_from_d3d12(
        const struct vkd3d_format *format, uint32_t plane_idx)
{
    VkImageAspectFlags aspect_mask = format->vk_aspect_mask;
    uint32_t i;

    /* For all formats we currently handle, the n-th aspect bit in Vulkan
     * corresponds to the n-th plane in D3D12, so isolate the respective
     * bit in the aspect mask. */
    for (i = 0; i < plane_idx; i++)
        aspect_mask &= aspect_mask - 1;

    if (!aspect_mask)
    {
        WARN("Invalid plane index %u for format %u.\n", plane_idx, format->vk_format);
        aspect_mask = format->vk_aspect_mask;
    }

    return aspect_mask & -aspect_mask;
}

VkImageSubresource vk_image_subresource_from_d3d12(
        const struct vkd3d_format *format, uint32_t subresource_idx,
        unsigned int miplevel_count, unsigned int layer_count,
        bool all_aspects)
{
    VkImageSubresource subresource;

    subresource.aspectMask = format->vk_aspect_mask;
    subresource.mipLevel = subresource_idx % miplevel_count;
    subresource.arrayLayer = (subresource_idx / miplevel_count) % layer_count;

    if (!all_aspects)
    {
        subresource.aspectMask = vk_image_aspect_flags_from_d3d12(
                format, subresource_idx / (miplevel_count * layer_count));
    }

    return subresource;
}

VkImageSubresource d3d12_resource_get_vk_subresource(const struct d3d12_resource *resource,
        uint32_t subresource_idx, bool all_aspects)
{
    return vk_image_subresource_from_d3d12(
            resource->format, subresource_idx,
            resource->desc.MipLevels, d3d12_resource_desc_get_layer_count(&resource->desc),
            all_aspects);
}

static HRESULT d3d12_validate_resource_flags(D3D12_RESOURCE_FLAGS flags)
{
    unsigned int unknown_flags = flags & ~(D3D12_RESOURCE_FLAG_NONE
            | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
            | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
            | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
            | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE
            | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER
            | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);

    if (unknown_flags)
        FIXME("Unknown resource flags %#x.\n", unknown_flags);

    if ((flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) && (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
    {
        ERR("ALLOW_SIMULTANEOUS_ACCESS and ALLOW_DEPTH_STENCIL is not allowed.\n");
        return E_INVALIDARG;
    }

    if ((flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) && (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
    {
        ERR("ALLOW_UNORDERED_ACCESS and ALLOW_DEPTH_STENCIL is not allowed.\n");
        return E_INVALIDARG;
    }

    return S_OK;
}

static bool d3d12_resource_validate_texture_format(const D3D12_RESOURCE_DESC *desc,
        const struct vkd3d_format *format)
{
    if (!vkd3d_format_is_compressed(format))
        return true;

    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && format->block_height > 1)
    {
        WARN("1D texture with a format block height > 1.\n");
        return false;
    }

    if (align(desc->Width, format->block_width) != desc->Width
            || align(desc->Height, format->block_height) != desc->Height)
    {
        WARN("Invalid size %"PRIu64"x%u for block compressed format %#x.\n",
                desc->Width, desc->Height, desc->Format);
        return false;
    }

    return true;
}

static bool d3d12_resource_validate_texture_alignment(const D3D12_RESOURCE_DESC *desc,
        const struct vkd3d_format *format)
{
    uint64_t estimated_size;

    if (!desc->Alignment)
        return true;

    if (desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT
            && desc->Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT
            && (desc->SampleDesc.Count == 1 || desc->Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT))
    {
        WARN("Invalid resource alignment %#"PRIx64".\n", desc->Alignment);
        return false;
    }

    if (desc->Alignment < D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
    {
        /* Windows uses the slice size to determine small alignment eligibility. DepthOrArraySize is ignored. */
        estimated_size = desc->Width * desc->Height * format->byte_count * format->block_byte_count
                / (format->block_width * format->block_height);
        if (estimated_size > D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
        {
            WARN("Invalid resource alignment %#"PRIx64" (required %#x).\n",
                    desc->Alignment, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
            return false;
        }
    }

    /* The size check for MSAA textures with D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT is probably
     * not important. The 4MB requirement is no longer universal and Vulkan has no such requirement. */

    return true;
}

HRESULT d3d12_resource_validate_desc(const D3D12_RESOURCE_DESC *desc, struct d3d12_device *device)
{
    const struct vkd3d_format *format;

    switch (desc->Dimension)
    {
        case D3D12_RESOURCE_DIMENSION_BUFFER:
            if (desc->MipLevels != 1)
            {
                WARN("Invalid miplevel count %u for buffer.\n", desc->MipLevels);
                return E_INVALIDARG;
            }

            if (desc->Format != DXGI_FORMAT_UNKNOWN || desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                    || desc->Height != 1 || desc->DepthOrArraySize != 1
                    || desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality != 0
                    || (desc->Alignment != 0 && desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT))
            {
                WARN("Invalid parameters for a buffer resource.\n");
                return E_INVALIDARG;
            }

            if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
            {
                WARN("D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS cannot be set for buffers.\n");
                return E_INVALIDARG;
            }
            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            if (desc->Height != 1)
            {
                WARN("1D texture with a height of %u.\n", desc->Height);
                return E_INVALIDARG;
            }
            /* Fall through. */
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            if (desc->SampleDesc.Count == 0)
            {
                WARN("Invalid sample count 0.\n");
                return E_INVALIDARG;
            }

            if (!(format = vkd3d_format_from_d3d12_resource_desc(device, desc, 0)))
            {
                WARN("Invalid format %#x.\n", desc->Format);
                return E_INVALIDARG;
            }

            if (!d3d12_resource_validate_texture_format(desc, format)
                    || !d3d12_resource_validate_texture_alignment(desc, format))
                return E_INVALIDARG;
            break;

        default:
            WARN("Invalid resource dimension %#x.\n", desc->Dimension);
            return E_INVALIDARG;
    }

    return d3d12_validate_resource_flags(desc->Flags);
}

static HRESULT d3d12_resource_validate_heap_properties(const D3D12_RESOURCE_DESC *desc,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_RESOURCE_STATES initial_state)
{
    if (heap_properties->Type == D3D12_HEAP_TYPE_UPLOAD
            || heap_properties->Type == D3D12_HEAP_TYPE_READBACK)
    {
        if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            WARN("Textures cannot be created on upload/readback heaps.\n");
            return E_INVALIDARG;
        }

        if (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        {
            WARN("Render target and unordered access buffers cannot be created on upload/readback heaps.\n");
            return E_INVALIDARG;
        }
    }

    if (heap_properties->Type == D3D12_HEAP_TYPE_UPLOAD && initial_state != D3D12_RESOURCE_STATE_GENERIC_READ)
    {
        WARN("For D3D12_HEAP_TYPE_UPLOAD the state must be D3D12_RESOURCE_STATE_GENERIC_READ.\n");
        return E_INVALIDARG;
    }
    if (heap_properties->Type == D3D12_HEAP_TYPE_READBACK && initial_state != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        WARN("For D3D12_HEAP_TYPE_READBACK the state must be D3D12_RESOURCE_STATE_COPY_DEST.\n");
        return E_INVALIDARG;
    }

    if (desc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
    {
        /* ROW_MAJOR textures are severely restricted in D3D12.
         * See test_map_texture_validation() for details. */
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            if (!(desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER))
            {
                WARN("ALLOW_CROSS_ADAPTER flag must be set to use ROW_MAJOR layout on textures.\n");
                return E_INVALIDARG;
            }

            if (desc->MipLevels > 1 || desc->DepthOrArraySize > 1)
            {
                WARN("For ROW_MAJOR textures, MipLevels and DepthOrArraySize must be 1.\n");
                return E_INVALIDARG;
            }

            if (heap_properties->Type == D3D12_HEAP_TYPE_CUSTOM &&
                    heap_properties->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
            {
                WARN("ROW_MAJOR textures cannot be CPU visible with CUSTOM heaps.\n");
                return E_INVALIDARG;
            }
        }
        else if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
                desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        {
            WARN("1D and 3D textures cannot be ROW_MAJOR layout.\n");
            return E_INVALIDARG;
        }
    }

    return S_OK;
}

static HRESULT d3d12_resource_validate_create_info(const D3D12_RESOURCE_DESC *desc,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value, struct d3d12_device *device)
{
    HRESULT hr;

    if (FAILED(hr = d3d12_resource_validate_desc(desc, device)))
        return hr;

    if (heap_properties)
    {
        if (FAILED(hr = d3d12_resource_validate_heap_properties(desc, heap_properties, initial_state)))
            return hr;
    }

    if (optimized_clear_value)
    {
        if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            WARN("Optimized clear value must be NULL for buffers.\n");
            return E_INVALIDARG;
        }

        WARN("Ignoring optimized clear value.\n");
    }

    if (!is_valid_resource_state(initial_state))
    {
        WARN("Invalid initial resource state %#x.\n", initial_state);
        return E_INVALIDARG;
    }

    return S_OK;
}

static HRESULT d3d12_resource_bind_sparse_metadata(struct d3d12_resource *resource,
        struct d3d12_device *device, struct d3d12_sparse_info *sparse)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkSparseImageMemoryRequirements *sparse_requirements = NULL;
    VkSparseImageOpaqueMemoryBindInfo opaque_bind;
    VkMemoryRequirements memory_requirements;
    VkSparseMemoryBind *memory_binds = NULL;
    struct vkd3d_queue *vkd3d_queue = NULL;
    uint32_t sparse_requirement_count;
    VkQueue vk_queue = VK_NULL_HANDLE;
    unsigned int i, j, k, bind_count;
    VkBindSparseInfo bind_info;
    VkDeviceSize metadata_size;
    HRESULT hr = S_OK;
    VkResult vr;

    if (d3d12_resource_is_buffer(resource))
        return S_OK;

    /* We expect the metadata aspect for image resources to be uncommon on most
     * drivers, so most of the time we'll just return early. The implementation
     * is therefore aimed at simplicity, and not very well tested in practice. */
    VK_CALL(vkGetImageSparseMemoryRequirements(device->vk_device,
        resource->res.vk_image, &sparse_requirement_count, NULL));

    if (!(sparse_requirements = vkd3d_malloc(sparse_requirement_count * sizeof(*sparse_requirements))))
    {
        ERR("Failed to allocate sparse memory requirement array.\n");
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    VK_CALL(vkGetImageSparseMemoryRequirements(device->vk_device,
        resource->res.vk_image, &sparse_requirement_count, sparse_requirements));

    /* Find out how much memory and how many bind infos we need */
    metadata_size = 0;
    bind_count = 0;

    for (i = 0; i < sparse_requirement_count; i++)
    {
        const VkSparseImageMemoryRequirements *req = &sparse_requirements[i];

        if (req->formatProperties.aspectMask & VK_IMAGE_ASPECT_METADATA_BIT)
        {
            uint32_t layer_count = 1;

            if (!(req->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT))
                layer_count = d3d12_resource_desc_get_layer_count(&resource->desc);

            metadata_size *= layer_count * req->imageMipTailSize;
            bind_count += layer_count;
        }
    }

    if (!metadata_size)
        goto cleanup;

    /* Allocate memory for metadata mip tail */
    TRACE("Allocating sparse metadata for resource %p.\n", resource);

    VK_CALL(vkGetImageMemoryRequirements(device->vk_device, resource->res.vk_image, &memory_requirements));

    if ((vr = vkd3d_allocate_device_memory(device, metadata_size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            memory_requirements.memoryTypeBits, NULL, &sparse->vk_metadata_memory)))
    {
        ERR("Failed to allocate device memory for sparse metadata, vr %d.\n", vr);
        hr = hresult_from_vk_result(vr);
        goto cleanup;
    }

    /* Fill in opaque memory bind info */
    if (!(memory_binds = vkd3d_malloc(bind_count * sizeof(*memory_binds))))
    {
        ERR("Failed to allocate sparse memory bind info array.\n");
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    metadata_size = 0;

    for (i = 0, j = 0; i < sparse_requirement_count; i++)
    {
        const VkSparseImageMemoryRequirements *req = &sparse_requirements[i];

        if (req->formatProperties.aspectMask & VK_IMAGE_ASPECT_METADATA_BIT)
        {
            uint32_t layer_count = 1;

            if (!(req->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT))
                layer_count = d3d12_resource_desc_get_layer_count(&resource->desc);

            for (k = 0; k < layer_count; k++)
            {
                VkSparseMemoryBind *bind = &memory_binds[j++];
                bind->resourceOffset = req->imageMipTailOffset + req->imageMipTailStride * k;
                bind->size = req->imageMipTailSize;
                bind->memory = sparse->vk_metadata_memory.vk_memory;
                bind->memoryOffset = metadata_size;
                bind->flags = VK_SPARSE_MEMORY_BIND_METADATA_BIT;

                metadata_size += req->imageMipTailSize;
            }
        }
    }

    /* Bind metadata memory to the image */
    opaque_bind.image = resource->res.vk_image;
    opaque_bind.bindCount = bind_count;
    opaque_bind.pBinds = memory_binds;

    bind_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
    bind_info.pNext = NULL;
    bind_info.waitSemaphoreCount = 0;
    bind_info.pWaitSemaphores = NULL;
    bind_info.bufferBindCount = 0;
    bind_info.pBufferBinds = NULL;
    bind_info.imageOpaqueBindCount = 1;
    bind_info.pImageOpaqueBinds = &opaque_bind;
    bind_info.imageBindCount = 0;
    bind_info.pImageBinds = NULL;
    bind_info.signalSemaphoreCount = 0;
    bind_info.pSignalSemaphores = NULL;

    vkd3d_queue = device->queue_families[VKD3D_QUEUE_FAMILY_SPARSE_BINDING]->queues[0];

    if (!(vk_queue = vkd3d_queue_acquire(vkd3d_queue)))
    {
        ERR("Failed to acquire queue %p.\n", vkd3d_queue);
        goto cleanup;
    }

    if ((vr = VK_CALL(vkQueueBindSparse(vk_queue, 1, &bind_info, VK_NULL_HANDLE))) < 0)
    {
        ERR("Failed to bind sparse metadata to image, vr %d.\n", vr);
        hr = hresult_from_vk_result(vr);
        goto cleanup;
    }

    /* The application is free to use or destroy the resource
     * immediately after creation, so we need to wait for the
     * sparse binding operation to finish on the GPU. */
    if ((vr = VK_CALL(vkQueueWaitIdle(vk_queue))))
    {
        ERR("Failed to wait for sparse binding to complete.\n");
        hr = hresult_from_vk_result(vr);
    }

cleanup:
    if (vkd3d_queue && vk_queue)
        vkd3d_queue_release(vkd3d_queue);

    vkd3d_free(sparse_requirements);
    vkd3d_free(memory_binds);
    return hr;
}

static HRESULT d3d12_resource_init_sparse_info(struct d3d12_resource *resource,
        struct d3d12_device *device, struct d3d12_sparse_info *sparse)
{
    VkSparseImageMemoryRequirements vk_memory_requirements;
    unsigned int i, subresource;
    VkOffset3D tile_offset;
    HRESULT hr;

    memset(sparse, 0, sizeof(*sparse));

    if (!(resource->flags & VKD3D_RESOURCE_RESERVED))
        return S_OK;

    sparse->tiling_count = d3d12_resource_desc_get_sub_resource_count_per_plane(&resource->desc);
    sparse->tile_count = 0;

    if (!(sparse->tilings = vkd3d_malloc(sparse->tiling_count * sizeof(*sparse->tilings))))
    {
        ERR("Failed to allocate subresource tiling info array.\n");
        return E_OUTOFMEMORY;
    }

    d3d12_resource_get_tiling(device, resource, &sparse->tile_count, &sparse->packed_mips,
            &sparse->tile_shape, sparse->tilings, &vk_memory_requirements);

    if (!(sparse->tiles = vkd3d_malloc(sparse->tile_count * sizeof(*sparse->tiles))))
    {
        ERR("Failed to allocate tile mapping array.\n");
        return E_OUTOFMEMORY;
    }

    tile_offset.x = 0;
    tile_offset.y = 0;
    tile_offset.z = 0;
    subresource = 0;

    for (i = 0; i < sparse->tile_count; i++)
    {
        if (d3d12_resource_is_buffer(resource))
        {
            VkDeviceSize offset = VKD3D_TILE_SIZE * i;
            sparse->tiles[i].buffer.offset = offset;
            sparse->tiles[i].buffer.length = min(VKD3D_TILE_SIZE, resource->desc.Width - offset);
        }
        else if (sparse->packed_mips.NumPackedMips && i >= sparse->packed_mips.StartTileIndexInOverallResource)
        {
            VkDeviceSize offset = VKD3D_TILE_SIZE * (i - sparse->packed_mips.StartTileIndexInOverallResource);
            sparse->tiles[i].buffer.offset = vk_memory_requirements.imageMipTailOffset + offset;
            sparse->tiles[i].buffer.length = min(VKD3D_TILE_SIZE, vk_memory_requirements.imageMipTailSize - offset);
        }
        else
        {
            struct d3d12_sparse_image_region *region = &sparse->tiles[i].image;
            VkExtent3D block_extent = vk_memory_requirements.formatProperties.imageGranularity;
            VkExtent3D mip_extent;

            assert(subresource < sparse->tiling_count && sparse->tilings[subresource].WidthInTiles &&
                    sparse->tilings[subresource].HeightInTiles && sparse->tilings[subresource].DepthInTiles);

            region->subresource.aspectMask = vk_memory_requirements.formatProperties.aspectMask;
            region->subresource.mipLevel = subresource % resource->desc.MipLevels;
            region->subresource.arrayLayer = subresource / resource->desc.MipLevels;
            region->subresource_index = subresource;

            region->offset.x = tile_offset.x * block_extent.width;
            region->offset.y = tile_offset.y * block_extent.height;
            region->offset.z = tile_offset.z * block_extent.depth;

            mip_extent.width = d3d12_resource_desc_get_width(&resource->desc, region->subresource.mipLevel);
            mip_extent.height = d3d12_resource_desc_get_height(&resource->desc, region->subresource.mipLevel);
            mip_extent.depth = d3d12_resource_desc_get_depth(&resource->desc, region->subresource.mipLevel);

            region->extent.width = min(block_extent.width, mip_extent.width - region->offset.x);
            region->extent.height = min(block_extent.height, mip_extent.height - region->offset.y);
            region->extent.depth = min(block_extent.depth, mip_extent.depth - region->offset.z);

            if (++tile_offset.x == (int32_t)sparse->tilings[subresource].WidthInTiles)
            {
                tile_offset.x = 0;
                if (++tile_offset.y == (int32_t)sparse->tilings[subresource].HeightInTiles)
                {
                    tile_offset.y = 0;
                    if (++tile_offset.z == (int32_t)sparse->tilings[subresource].DepthInTiles)
                    {
                        tile_offset.z = 0;

                        /* Find next subresource that is not part of the packed mip tail */
                        while ((++subresource % resource->desc.MipLevels) >= sparse->packed_mips.NumStandardMips)
                            continue;
                    }
                }
            }
        }

        sparse->tiles[i].vk_memory = VK_NULL_HANDLE;
        sparse->tiles[i].vk_offset = 0;
    }

    if (FAILED(hr = d3d12_resource_bind_sparse_metadata(resource, device, sparse)))
        return hr;

    return S_OK;
}

static void d3d12_resource_destroy(struct d3d12_resource *resource, struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    vkd3d_view_map_destroy(&resource->view_map, resource->device);

    vkd3d_descriptor_debug_unregister_cookie(device->descriptor_qa_global_info, resource->res.cookie);

    if (resource->flags & VKD3D_RESOURCE_EXTERNAL)
        return;

    if (resource->flags & VKD3D_RESOURCE_RESERVED)
    {
        vkd3d_free_device_memory(device, &resource->sparse.vk_metadata_memory);
        vkd3d_free(resource->sparse.tiles);
        vkd3d_free(resource->sparse.tilings);

        if (resource->res.va)
        {
            vkd3d_va_map_remove(&device->memory_allocator.va_map, &resource->res);

            if (!device->device_info.buffer_device_address_features.bufferDeviceAddress)
                vkd3d_va_map_free_fake_va(&device->memory_allocator.va_map, resource->res.va, resource->res.size);
        }
    }

    if (d3d12_resource_is_texture(resource))
        VK_CALL(vkDestroyImage(device->vk_device, resource->res.vk_image, NULL));
    else if (resource->flags & VKD3D_RESOURCE_RESERVED)
        VK_CALL(vkDestroyBuffer(device->vk_device, resource->res.vk_buffer, NULL));

    if ((resource->flags & VKD3D_RESOURCE_ALLOCATION) && resource->mem.device_allocation.vk_memory)
        vkd3d_free_memory(device, &device->memory_allocator, &resource->mem);

    if (resource->vrs_view)
        VK_CALL(vkDestroyImageView(device->vk_device, resource->vrs_view, NULL));

    vkd3d_private_store_destroy(&resource->private_store);
    d3d12_device_release(resource->device);
    vkd3d_free(resource);
}

static HRESULT d3d12_resource_create_vk_resource(struct d3d12_resource *resource, struct d3d12_device *device)
{
    const D3D12_HEAP_PROPERTIES *heap_properties;
    HRESULT hr;

    heap_properties = resource->flags & VKD3D_RESOURCE_RESERVED
        ? NULL : &resource->heap_properties;

    if (resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        if (FAILED(hr = vkd3d_create_buffer(device, heap_properties,
                D3D12_HEAP_FLAG_NONE, &resource->desc, &resource->res.vk_buffer)))
            return hr;
    }
    else
    {
        resource->initial_layout_transition = 1;

        if (!resource->desc.MipLevels)
            resource->desc.MipLevels = max_miplevel_count(&resource->desc);

        if (FAILED(hr = vkd3d_create_image(device, heap_properties,
                D3D12_HEAP_FLAG_NONE, &resource->desc, resource, &resource->res.vk_image)))
            return hr;
    }

    return S_OK;
}

static HRESULT d3d12_resource_create(struct d3d12_device *device, uint32_t flags,
        const D3D12_RESOURCE_DESC *desc, const D3D12_HEAP_PROPERTIES *heap_properties,
        D3D12_HEAP_FLAGS heap_flags, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value, struct d3d12_resource **resource)
{
    struct d3d12_resource *object;
    HRESULT hr;

    if (FAILED(hr = d3d12_resource_validate_create_info(desc,
            heap_properties, initial_state, optimized_clear_value, device)))
        return hr;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    memset(object, 0, sizeof(*object));
    object->ID3D12Resource_iface.lpVtbl = &d3d12_resource_vtbl;

    if (FAILED(hr = vkd3d_view_map_init(&object->view_map)))
    {
        vkd3d_free(object);
        return hr;
    }

    if (FAILED(hr = vkd3d_private_store_init(&object->private_store)))
    {
        vkd3d_view_map_destroy(&object->view_map, device);
        vkd3d_free(object);
        return hr;
    }

    object->refcount = 1;
    object->internal_refcount = 1;
    object->desc = *desc;
    object->device = device;
    object->flags = flags;
    object->format = vkd3d_format_from_d3d12_resource_desc(device, desc, 0);
    object->res.cookie = vkd3d_allocate_cookie();
#ifdef VKD3D_ENABLE_DESCRIPTOR_QA
    object->view_map.resource_cookie = object->res.cookie;
#endif

    /* RTAS are "special" buffers. They can never transition out of this state. */
    if (initial_state == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
        object->flags |= VKD3D_RESOURCE_ACCELERATION_STRUCTURE;
    object->initial_state = initial_state;

    if (heap_properties)
        object->heap_properties = *heap_properties;
    object->heap_flags = heap_flags;

    d3d12_device_add_ref(device);

    vkd3d_descriptor_debug_register_resource_cookie(device->descriptor_qa_global_info,
            object->res.cookie, desc);

    *resource = object;
    return S_OK;
}

HRESULT d3d12_resource_create_committed(struct d3d12_device *device, const D3D12_RESOURCE_DESC *desc,
        const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS heap_flags, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value, struct d3d12_resource **resource)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct d3d12_resource *object;
    HRESULT hr;

    if (FAILED(hr = d3d12_resource_create(device, VKD3D_RESOURCE_COMMITTED | VKD3D_RESOURCE_ALLOCATION,
            desc, heap_properties, heap_flags, initial_state, optimized_clear_value, &object)))
        return hr;

    if (d3d12_resource_is_texture(object))
    {
        VkMemoryDedicatedRequirements dedicated_requirements;
        struct vkd3d_allocate_memory_info allocate_info;
        VkMemoryDedicatedAllocateInfo dedicated_info;
        VkImageMemoryRequirementsInfo2 image_info;
        VkMemoryRequirements2 memory_requirements;
        bool use_dedicated_allocation;
        VkResult vr;

        if (FAILED(hr = d3d12_resource_create_vk_resource(object, device)))
            goto fail;

        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
        image_info.pNext = NULL;
        image_info.image = object->res.vk_image;

        dedicated_requirements.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
        dedicated_requirements.pNext = NULL;

        memory_requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        memory_requirements.pNext = &dedicated_requirements;

        VK_CALL(vkGetImageMemoryRequirements2(device->vk_device, &image_info, &memory_requirements));

        if (!(use_dedicated_allocation = dedicated_requirements.prefersDedicatedAllocation))
        {
            const uint32_t type_mask = memory_requirements.memoryRequirements.memoryTypeBits & device->memory_info.global_mask;
            const struct vkd3d_memory_info_domain *domain = d3d12_device_get_memory_info_domain(device, heap_properties);
            use_dedicated_allocation = (type_mask & domain->buffer_type_mask) != type_mask;
        }

        memset(&allocate_info, 0, sizeof(allocate_info));
        allocate_info.memory_requirements = memory_requirements.memoryRequirements;
        allocate_info.heap_properties = *heap_properties;
        allocate_info.heap_flags = heap_flags;

        if (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            allocate_info.heap_flags |= D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
        else
            allocate_info.heap_flags |= D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;

        if (use_dedicated_allocation)
        {
            dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicated_info.pNext = NULL;
            dedicated_info.image = object->res.vk_image;
            dedicated_info.buffer = VK_NULL_HANDLE;
            allocate_info.pNext = &dedicated_info;
            allocate_info.flags = VKD3D_ALLOCATION_FLAG_DEDICATED;
        }
        else
        {
            /* We want to allow suballocations and we need the allocation to
             * be cleared to zero, which only works if we allow buffers */
            allocate_info.heap_flags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
            allocate_info.flags = VKD3D_ALLOCATION_FLAG_GLOBAL_BUFFER;
        }

        if (FAILED(hr = vkd3d_allocate_memory(device, &device->memory_allocator, &allocate_info, &object->mem)))
            goto fail;

        if ((vr = VK_CALL(vkBindImageMemory(device->vk_device, object->res.vk_image,
                object->mem.device_allocation.vk_memory, object->mem.offset))))
        {
            ERR("Failed to bind image memory, vr %d.\n", vr);
            hr = hresult_from_vk_result(vr);
            goto fail;
        }

        if (vkd3d_resource_can_be_vrs(device, heap_properties, desc))
        {
            /* Make the implicit VRS view here... */
            if (FAILED(hr = vkd3d_resource_make_vrs_view(device, object->res.vk_image, &object->vrs_view)))
                goto fail;
        }
    }
    else
    {
        struct vkd3d_allocate_heap_memory_info allocate_info;

        memset(&allocate_info, 0, sizeof(allocate_info));
        allocate_info.heap_desc.Properties = *heap_properties;
        allocate_info.heap_desc.Alignment = desc->Alignment ? desc->Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        allocate_info.heap_desc.SizeInBytes = align(desc->Width, allocate_info.heap_desc.Alignment);
        allocate_info.heap_desc.Flags = heap_flags | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

        if (FAILED(hr = vkd3d_allocate_heap_memory(device,
                &device->memory_allocator, &allocate_info, &object->mem)))
            goto fail;

        object->res.vk_buffer = object->mem.resource.vk_buffer;
        object->res.va = object->mem.resource.va;
    }

    *resource = object;
    return S_OK;

fail:
    d3d12_resource_destroy(object, device);
    return hr;
}

static HRESULT d3d12_resource_validate_heap(const D3D12_RESOURCE_DESC *resource_desc, struct d3d12_heap *heap)
{
    D3D12_HEAP_FLAGS deny_flag;

    if (resource_desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        deny_flag = D3D12_HEAP_FLAG_DENY_BUFFERS;
    else if (resource_desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        deny_flag = D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
    else
        deny_flag = D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;

    if (heap->desc.Flags & deny_flag)
    {
        WARN("Cannot create placed resource on heap that denies resource category %#x.\n", deny_flag);
        return E_INVALIDARG;
    }

    if ((heap->desc.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) &&
            !(resource_desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER))
    {
        ERR("Must declare ALLOW_CROSS_ADAPTER resource flag when heap is cross adapter.\n");
        return E_INVALIDARG;
    }

    return S_OK;
}

HRESULT d3d12_resource_create_placed(struct d3d12_device *device, const D3D12_RESOURCE_DESC *desc,
        struct d3d12_heap *heap, uint64_t heap_offset, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value, struct d3d12_resource **resource)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkMemoryRequirements memory_requirements;
    struct d3d12_resource *object;
    VkResult vr;
    HRESULT hr;

    if (FAILED(hr = d3d12_resource_validate_heap(desc, heap)))
        return hr;

    if (heap->allocation.device_allocation.vk_memory == VK_NULL_HANDLE)
    {
        WARN("Placing resource on heap with no memory backing it. Falling back to committed resource.\n");
        if (FAILED(hr = d3d12_resource_create_committed(device, desc, &heap->desc.Properties,
                heap->desc.Flags & ~(D3D12_HEAP_FLAG_DENY_BUFFERS |
                        D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES |
                        D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES),
                initial_state, optimized_clear_value, resource)))
        {
            ERR("Failed to create fallback committed resource.\n");
        }
        return hr;
    }

    if (FAILED(hr = d3d12_resource_create(device, VKD3D_RESOURCE_PLACED, desc,
            &heap->desc.Properties, heap->desc.Flags, initial_state, optimized_clear_value, &object)))
        return hr;

    object->heap = heap;

    if (d3d12_resource_is_texture(object))
    {
        if (FAILED(hr = d3d12_resource_create_vk_resource(object, device)))
            goto fail;

        /* Align manually. This works because we padded the required allocation size reported to the app. */
        VK_CALL(vkGetImageMemoryRequirements(device->vk_device, object->res.vk_image, &memory_requirements));
        heap_offset = align(heap_offset, memory_requirements.alignment);

        if (heap_offset + memory_requirements.size > heap->allocation.resource.size)
        {
            ERR("Heap too small for the texture (heap=%"PRIu64", res=%"PRIu64".\n",
                heap->allocation.resource.size, heap_offset + memory_requirements.size);
            hr = E_INVALIDARG;
            goto fail;
        }
    }
    else
    {
        if (heap_offset + desc->Width > heap->allocation.resource.size)
        {
            ERR("Heap too small for the buffer (heap=%"PRIu64", res=%"PRIu64".\n",
                heap->allocation.resource.size, heap_offset + desc->Width);
            hr = E_INVALIDARG;
            goto fail;
        }
    }

    vkd3d_memory_allocation_slice(&object->mem, &heap->allocation, heap_offset, 0);

    if (d3d12_resource_is_texture(object))
    {
        if ((vr = VK_CALL(vkBindImageMemory(device->vk_device, object->res.vk_image,
                object->mem.device_allocation.vk_memory, object->mem.offset)) < 0))
        {
            ERR("Failed to bind image memory, vr %d.\n", vr);
            hr = hresult_from_vk_result(vr);
            goto fail;
        }
    }
    else
    {
        object->res.vk_buffer = object->mem.resource.vk_buffer;
        object->res.va = object->mem.resource.va;
    }

    if (vkd3d_resource_can_be_vrs(device, &heap->desc.Properties, desc))
    {
        /* Make the implicit VRS view here... */
        if (FAILED(hr = vkd3d_resource_make_vrs_view(device, object->res.vk_image, &object->vrs_view)))
            goto fail;
    }

    *resource = object;
    return S_OK;

fail:
    d3d12_resource_destroy(object, device);
    return hr;
}

HRESULT d3d12_resource_create_reserved(struct d3d12_device *device,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value, struct d3d12_resource **resource)
{
    struct d3d12_resource *object;
    HRESULT hr;

    if (FAILED(hr = d3d12_resource_create(device, VKD3D_RESOURCE_RESERVED, desc,
            NULL, D3D12_HEAP_FLAG_NONE, initial_state, optimized_clear_value, &object)))
        return hr;

    if (FAILED(hr = d3d12_resource_create_vk_resource(object, device)))
        goto fail;

    if (FAILED(hr = d3d12_resource_init_sparse_info(object, device, &object->sparse)))
        goto fail;

    if (d3d12_resource_is_buffer(object))
    {
        object->res.size = object->desc.Width;

        if (device->device_info.buffer_device_address_features.bufferDeviceAddress)
            object->res.va = vkd3d_get_buffer_device_address(device, object->res.vk_buffer);
        else
            object->res.va = vkd3d_va_map_alloc_fake_va(&device->memory_allocator.va_map, object->res.size);

        if (!object->res.va)
        {
            ERR("Failed to get VA for sparse resource.\n");
            return E_FAIL;
        }

        vkd3d_va_map_insert(&device->memory_allocator.va_map, &object->res);
    }

    *resource = object;
    return S_OK;

fail:
    d3d12_resource_destroy(object, device);
    return hr;
}

VKD3D_EXPORT HRESULT vkd3d_create_image_resource(ID3D12Device *device,
        const struct vkd3d_image_resource_create_info *create_info, ID3D12Resource **resource)
{
    struct d3d12_device *d3d12_device = impl_from_ID3D12Device((d3d12_device_iface *)device);
    struct d3d12_resource *object;
    HRESULT hr;

    TRACE("device %p, create_info %p, resource %p.\n", device, create_info, resource);

    if (!create_info || !resource)
        return E_INVALIDARG;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    memset(object, 0, sizeof(*object));

    object->ID3D12Resource_iface.lpVtbl = &d3d12_resource_vtbl;
    object->refcount = 1;
    object->internal_refcount = 1;
    object->desc = create_info->desc;
    object->res.vk_image = create_info->vk_image;
    object->flags = create_info->flags;
    object->flags |= VKD3D_RESOURCE_EXTERNAL;
    object->initial_layout_transition = 1;
    object->common_layout = vk_common_image_layout_from_d3d12_desc(&object->desc);

    memset(&object->sparse, 0, sizeof(object->sparse));

    object->format = vkd3d_format_from_d3d12_resource_desc(d3d12_device, &create_info->desc, 0);

    if (FAILED(hr = vkd3d_view_map_init(&object->view_map)))
    {
        vkd3d_free(object);
        return hr;
    }

    if (FAILED(hr = vkd3d_private_store_init(&object->private_store)))
    {
        vkd3d_free(object);
        return hr;
    }

    d3d12_device_add_ref(object->device = d3d12_device);

    TRACE("Created resource %p.\n", object);

    *resource = (ID3D12Resource *)&object->ID3D12Resource_iface;

    return S_OK;
}

VKD3D_EXPORT ULONG vkd3d_resource_incref(ID3D12Resource *resource)
{
    TRACE("resource %p.\n", resource);
    return d3d12_resource_incref(impl_from_ID3D12Resource(resource));
}

VKD3D_EXPORT ULONG vkd3d_resource_decref(ID3D12Resource *resource)
{
    TRACE("resource %p.\n", resource);
    return d3d12_resource_decref(impl_from_ID3D12Resource(resource));
}

/* CBVs, SRVs, UAVs */
static struct vkd3d_view *vkd3d_view_create(enum vkd3d_view_type type)
{
    struct vkd3d_view *view;

    if ((view = vkd3d_malloc(sizeof(*view))))
    {
        view->refcount = 1;
        view->type = type;
        view->cookie = vkd3d_allocate_cookie();
    }
    return view;
}

void vkd3d_view_incref(struct vkd3d_view *view)
{
    InterlockedIncrement(&view->refcount);
}

static void vkd3d_view_destroy(struct vkd3d_view *view, struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    TRACE("Destroying view %p.\n", view);

    vkd3d_descriptor_debug_unregister_cookie(device->descriptor_qa_global_info, view->cookie);

    switch (view->type)
    {
        case VKD3D_VIEW_TYPE_BUFFER:
            VK_CALL(vkDestroyBufferView(device->vk_device, view->vk_buffer_view, NULL));
            break;
        case VKD3D_VIEW_TYPE_IMAGE:
            VK_CALL(vkDestroyImageView(device->vk_device, view->vk_image_view, NULL));
            break;
        case VKD3D_VIEW_TYPE_SAMPLER:
            VK_CALL(vkDestroySampler(device->vk_device, view->vk_sampler, NULL));
            break;
        case VKD3D_VIEW_TYPE_ACCELERATION_STRUCTURE:
            VK_CALL(vkDestroyAccelerationStructureKHR(device->vk_device, view->vk_acceleration_structure, NULL));
            break;
        default:
            WARN("Unhandled view type %d.\n", view->type);
    }

    vkd3d_free(view);
}

void vkd3d_view_decref(struct vkd3d_view *view, struct d3d12_device *device)
{
    if (!InterlockedDecrement(&view->refcount))
        vkd3d_view_destroy(view, device);
}

static void d3d12_desc_copy_single(struct d3d12_desc *dst, struct d3d12_desc *src,
        struct d3d12_device *device)
{
    VkCopyDescriptorSet vk_copies[VKD3D_MAX_BINDLESS_DESCRIPTOR_SETS];
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct vkd3d_descriptor_data metadata = src->metadata;
    struct vkd3d_descriptor_binding binding;
    uint32_t set_mask, set_info_index;
    const VkDescriptorSet *src_sets;
    const VkDescriptorSet *dst_sets;
    VkCopyDescriptorSet *vk_copy;
    uint32_t copy_count = 0;
    bool needs_update;

    /* Only update the descriptor if something has changed */
    if (!(needs_update = (metadata.cookie != dst->metadata.cookie)))
    {
        /* We don't have a cookie for the UAV counter, so just force update if we have that.
         * If flags differ, we also need to update. E.g. happens if UAV counter flag is turned off.
         * We have no cookie for the UAV counter itself.
         * Lastly, if we have plain VkBuffers, offset/range might differ. */
        if ((metadata.flags & VKD3D_DESCRIPTOR_FLAG_RAW_VA_AUX_BUFFER) != 0 ||
            (metadata.flags != dst->metadata.flags))
        {
            needs_update = true;
        }
        else if (metadata.flags & VKD3D_DESCRIPTOR_FLAG_OFFSET_RANGE)
        {
            needs_update =
                    dst->info.buffer.offset != src->info.buffer.offset ||
                    dst->info.buffer.range != src->info.buffer.range;
        }
    }

    if (needs_update)
    {
        src_sets = src->heap->vk_descriptor_sets;
        dst_sets = dst->heap->vk_descriptor_sets;
        dst->metadata = metadata;
        dst->info = src->info;
        set_mask = metadata.set_info_mask;

        while (set_mask)
        {
            set_info_index = vkd3d_bitmask_iter32(&set_mask);
            binding = vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, set_info_index);

            vk_copy = &vk_copies[copy_count++];
            vk_copy->sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
            vk_copy->pNext = NULL;
            vk_copy->srcSet = src_sets[binding.set];
            vk_copy->srcBinding = binding.binding;
            vk_copy->srcArrayElement = src->heap_offset;
            vk_copy->dstSet = dst_sets[binding.set];
            vk_copy->dstBinding = binding.binding;
            vk_copy->dstArrayElement = dst->heap_offset;
            vk_copy->descriptorCount = 1;
        }

        if (metadata.flags & VKD3D_DESCRIPTOR_FLAG_RAW_VA_AUX_BUFFER)
        {
            if (dst->heap->raw_va_aux_buffer.host_ptr)
            {
                const VkDeviceAddress *src_vas = src->heap->raw_va_aux_buffer.host_ptr;
                VkDeviceAddress *dst_vas = dst->heap->raw_va_aux_buffer.host_ptr;
                dst_vas[dst->heap_offset] = src_vas[src->heap_offset];
            }
            else
            {
                binding = vkd3d_bindless_state_find_set(
                        &device->bindless_state, VKD3D_BINDLESS_SET_UAV | VKD3D_BINDLESS_SET_AUX_BUFFER);

                vk_copy = &vk_copies[copy_count++];
                vk_copy->sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
                vk_copy->pNext = NULL;
                vk_copy->srcSet = src->heap->vk_descriptor_sets[binding.set];
                vk_copy->srcBinding = binding.binding;
                vk_copy->srcArrayElement = src->heap_offset;
                vk_copy->dstSet = dst->heap->vk_descriptor_sets[binding.set];
                vk_copy->dstBinding = binding.binding;
                vk_copy->dstArrayElement = dst->heap_offset;
                vk_copy->descriptorCount = 1;
            }
        }

        if (copy_count)
            VK_CALL(vkUpdateDescriptorSets(device->vk_device, 0, NULL, copy_count, vk_copies));
    }

    if (metadata.flags & VKD3D_DESCRIPTOR_FLAG_BUFFER_OFFSET)
    {
        const struct vkd3d_bound_buffer_range *src_buffer_ranges = src->heap->buffer_ranges.host_ptr;
        struct vkd3d_bound_buffer_range *dst_buffer_ranges = dst->heap->buffer_ranges.host_ptr;
        dst_buffer_ranges[dst->heap_offset] = src_buffer_ranges[src->heap_offset];
    }
}

void d3d12_desc_copy_range(struct d3d12_desc *dst, struct d3d12_desc *src,
        unsigned int count, D3D12_DESCRIPTOR_HEAP_TYPE heap_type, struct d3d12_device *device)
{
    VkCopyDescriptorSet vk_copies[VKD3D_MAX_BINDLESS_DESCRIPTOR_SETS];
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct vkd3d_descriptor_binding binding;
    VkCopyDescriptorSet *vk_copy;
    uint32_t set_info_mask = 0;
    uint32_t copy_count = 0;
    uint32_t set_info_index;
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        set_info_mask |= src[i].metadata.set_info_mask;
        dst[i].metadata = src[i].metadata;
        dst[i].info = src[i].info;
    }

    while (set_info_mask)
    {
        set_info_index = vkd3d_bitmask_iter32(&set_info_mask);
        binding = vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, set_info_index);

        vk_copy = &vk_copies[copy_count++];
        vk_copy->sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
        vk_copy->pNext = NULL;
        vk_copy->srcSet = src->heap->vk_descriptor_sets[binding.set];
        vk_copy->srcBinding = binding.binding;
        vk_copy->srcArrayElement = src->heap_offset;
        vk_copy->dstSet = dst->heap->vk_descriptor_sets[binding.set];
        vk_copy->dstBinding = binding.binding;
        vk_copy->dstArrayElement = dst->heap_offset;
        vk_copy->descriptorCount = count;
    }

    if (heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        if (device->bindless_state.flags & VKD3D_RAW_VA_AUX_BUFFER)
        {
            const VkDeviceAddress *src_vas = src->heap->raw_va_aux_buffer.host_ptr;
            VkDeviceAddress *dst_vas = dst->heap->raw_va_aux_buffer.host_ptr;
            memcpy(dst_vas + dst->heap_offset, src_vas + src->heap_offset, sizeof(*dst_vas) * count);
        }
        else
        {
            binding = vkd3d_bindless_state_find_set(&device->bindless_state, VKD3D_BINDLESS_SET_UAV | VKD3D_BINDLESS_SET_AUX_BUFFER);

            vk_copy = &vk_copies[copy_count++];
            vk_copy->sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
            vk_copy->pNext = NULL;
            vk_copy->srcSet = src->heap->vk_descriptor_sets[binding.set];
            vk_copy->srcBinding = binding.binding;
            vk_copy->srcArrayElement = src->heap_offset;
            vk_copy->dstSet = dst->heap->vk_descriptor_sets[binding.set];
            vk_copy->dstBinding = binding.binding;
            vk_copy->dstArrayElement = dst->heap_offset;
            vk_copy->descriptorCount = count;
        }

        if (device->bindless_state.flags & (VKD3D_TYPED_OFFSET_BUFFER | VKD3D_SSBO_OFFSET_BUFFER))
        {
            const struct vkd3d_bound_buffer_range *src_ranges = src->heap->buffer_ranges.host_ptr;
            struct vkd3d_bound_buffer_range *dst_ranges = dst->heap->buffer_ranges.host_ptr;
            memcpy(dst_ranges + dst->heap_offset, src_ranges + src->heap_offset, sizeof(*dst_ranges) * count);
        }
    }

    if (copy_count)
        VK_CALL(vkUpdateDescriptorSets(device->vk_device, 0, NULL, copy_count, vk_copies));
}

void d3d12_desc_copy(struct d3d12_desc *dst, struct d3d12_desc *src,
        unsigned int count, D3D12_DESCRIPTOR_HEAP_TYPE heap_type, struct d3d12_device *device)
{
    unsigned int i;

#ifdef VKD3D_ENABLE_DESCRIPTOR_QA
    for (i = 0; i < count; i++)
    {
        vkd3d_descriptor_debug_copy_descriptor(
                dst[i].heap->descriptor_heap_info.host_ptr, dst[i].heap->cookie, dst[i].heap_offset,
                src[i].heap->descriptor_heap_info.host_ptr, src[i].heap->cookie, src[i].heap_offset,
                src[i].metadata.cookie);
    }
#endif

    if (device->bindless_state.flags & VKD3D_BINDLESS_MUTABLE_TYPE)
        d3d12_desc_copy_range(dst, src, count, heap_type, device);
    else
    {
        for (i = 0; i < count; i++)
            d3d12_desc_copy_single(dst + i, src + i, device);
    }
}

static VkDeviceSize vkd3d_get_required_texel_buffer_alignment(const struct d3d12_device *device,
        const struct vkd3d_format *format)
{
    const VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT *properties;
    const struct vkd3d_vulkan_info *vk_info = &device->vk_info;
    VkDeviceSize alignment;

    if (vk_info->EXT_texel_buffer_alignment)
    {
        properties = &vk_info->texel_buffer_alignment_properties;

        alignment = max(properties->storageTexelBufferOffsetAlignmentBytes,
                properties->uniformTexelBufferOffsetAlignmentBytes);

        if (properties->storageTexelBufferOffsetSingleTexelAlignment
                && properties->uniformTexelBufferOffsetSingleTexelAlignment)
        {
            assert(!vkd3d_format_is_compressed(format));
            return min(format->byte_count, alignment);
        }

        return alignment;
    }

    return vk_info->device_limits.minTexelBufferOffsetAlignment;
}

bool vkd3d_create_raw_r32ui_vk_buffer_view(struct d3d12_device *device,
        VkBuffer vk_buffer, VkDeviceSize offset, VkDeviceSize range, VkBufferView *vk_view)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct VkBufferViewCreateInfo view_desc;
    VkResult vr;

    if (offset % 4)
        FIXME("Offset %#"PRIx64" violates the required alignment 4.\n", offset);

    view_desc.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    view_desc.pNext = NULL;
    view_desc.flags = 0;
    view_desc.buffer = vk_buffer;
    view_desc.format = VK_FORMAT_R32_UINT;
    view_desc.offset = offset;
    view_desc.range = range;
    if ((vr = VK_CALL(vkCreateBufferView(device->vk_device, &view_desc, NULL, vk_view))) < 0)
        WARN("Failed to create Vulkan buffer view, vr %d.\n", vr);
    return vr == VK_SUCCESS;
}

static bool vkd3d_create_vk_buffer_view(struct d3d12_device *device,
        VkBuffer vk_buffer, const struct vkd3d_format *format,
        VkDeviceSize offset, VkDeviceSize range, VkBufferView *vk_view)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct VkBufferViewCreateInfo view_desc;
    VkDeviceSize alignment;
    VkResult vr;

    if (vkd3d_format_is_compressed(format))
    {
        WARN("Invalid format for buffer view %#x.\n", format->dxgi_format);
        return false;
    }

    alignment = vkd3d_get_required_texel_buffer_alignment(device, format);
    if (offset % alignment)
        FIXME("Offset %#"PRIx64" violates the required alignment %#"PRIx64".\n", offset, alignment);

    view_desc.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    view_desc.pNext = NULL;
    view_desc.flags = 0;
    view_desc.buffer = vk_buffer;
    view_desc.format = format->vk_format;
    view_desc.offset = offset;
    view_desc.range = range;
    if ((vr = VK_CALL(vkCreateBufferView(device->vk_device, &view_desc, NULL, vk_view))) < 0)
        WARN("Failed to create Vulkan buffer view, vr %d.\n", vr);
    return vr == VK_SUCCESS;
}

bool vkd3d_create_buffer_view(struct d3d12_device *device, const struct vkd3d_buffer_view_desc *desc, struct vkd3d_view **view)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct vkd3d_view *object;
    VkBufferView vk_view;

    if (!vkd3d_create_vk_buffer_view(device, desc->buffer, desc->format, desc->offset, desc->size, &vk_view))
        return false;

    if (!(object = vkd3d_view_create(VKD3D_VIEW_TYPE_BUFFER)))
    {
        VK_CALL(vkDestroyBufferView(device->vk_device, vk_view, NULL));
        return false;
    }

    object->vk_buffer_view = vk_view;
    object->format = desc->format;
    object->info.buffer.offset = desc->offset;
    object->info.buffer.size = desc->size;
    *view = object;
    return true;
}

bool vkd3d_create_acceleration_structure_view(struct d3d12_device *device, const struct vkd3d_buffer_view_desc *desc,
        struct vkd3d_view **view)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkAccelerationStructureKHR vk_acceleration_structure;
    VkAccelerationStructureCreateInfoKHR create_info;
    VkDeviceAddress buffer_address;
    VkDeviceAddress rtas_address;
    struct vkd3d_view *object;
    VkResult vr;

    create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    create_info.pNext = NULL;
    create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR;
    create_info.createFlags = 0;
    create_info.deviceAddress = 0;
    create_info.buffer = desc->buffer;
    create_info.offset = desc->offset;
    create_info.size = desc->size;

    vr = VK_CALL(vkCreateAccelerationStructureKHR(device->vk_device, &create_info, NULL, &vk_acceleration_structure));
    if (vr != VK_SUCCESS)
        return false;

    if (!(object = vkd3d_view_create(VKD3D_VIEW_TYPE_ACCELERATION_STRUCTURE)))
    {
        VK_CALL(vkDestroyAccelerationStructureKHR(device->vk_device, vk_acceleration_structure, NULL));
        return false;
    }

    /* Sanity check. Spec should guarantee this.
     * There is a note in the spec for vkGetAccelerationStructureDeviceAddressKHR:
     * The acceleration structure device address may be different from the
     * buffer device address corresponding to the acceleration structure's
     * start offset in its storage buffer for acceleration structure types
     * other than VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR. */
    buffer_address = vkd3d_get_buffer_device_address(device, desc->buffer) + desc->offset;
    rtas_address = vkd3d_get_acceleration_structure_device_address(device, vk_acceleration_structure);
    if (buffer_address != rtas_address)
    {
        FIXME("buffer_address = 0x%"PRIx64", rtas_address = 0x%"PRIx64".\n", buffer_address, rtas_address);
    }

    object->vk_acceleration_structure = vk_acceleration_structure;
    object->format = desc->format;
    object->info.buffer.offset = desc->offset;
    object->info.buffer.size = desc->size;
    *view = object;
    return true;
}

#define VKD3D_VIEW_RAW_BUFFER 0x1

static bool vkd3d_create_buffer_view_for_resource(struct d3d12_device *device,
        struct d3d12_resource *resource, DXGI_FORMAT view_format,
        unsigned int offset, unsigned int size, unsigned int structure_stride,
        unsigned int flags, struct vkd3d_view **view)
{
    const struct vkd3d_format *format;
    struct vkd3d_view_key key;
    VkDeviceSize element_size;

    if (view_format == DXGI_FORMAT_R32_TYPELESS && (flags & VKD3D_VIEW_RAW_BUFFER))
    {
        format = vkd3d_get_format(device, DXGI_FORMAT_R32_UINT, false);
        element_size = format->byte_count;
    }
    else if (view_format == DXGI_FORMAT_UNKNOWN && structure_stride)
    {
        format = vkd3d_get_format(device, DXGI_FORMAT_R32_UINT, false);
        element_size = structure_stride;
    }
    else if ((format = vkd3d_format_from_d3d12_resource_desc(device, &resource->desc, view_format)))
    {
        element_size = format->byte_count;
    }
    else
    {
        WARN("Failed to find format for %#x.\n", resource->desc.Format);
        return false;
    }

    assert(d3d12_resource_is_buffer(resource));

    key.view_type = VKD3D_VIEW_TYPE_BUFFER;
    key.u.buffer.buffer = resource->res.vk_buffer;
    key.u.buffer.format = format;
    key.u.buffer.offset = resource->mem.offset + offset * element_size;
    key.u.buffer.size = size * element_size;

    return !!(*view = vkd3d_view_map_create_view(&resource->view_map, device, &key));
}

static void vkd3d_set_view_swizzle_for_format(VkComponentMapping *components,
        const struct vkd3d_format *format, bool allowed_swizzle)
{
    components->r = VK_COMPONENT_SWIZZLE_R;
    components->g = VK_COMPONENT_SWIZZLE_G;
    components->b = VK_COMPONENT_SWIZZLE_B;
    components->a = VK_COMPONENT_SWIZZLE_A;

    if (format->vk_aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT)
    {
        if (allowed_swizzle)
        {
            components->r = VK_COMPONENT_SWIZZLE_ZERO;
            components->g = VK_COMPONENT_SWIZZLE_R;
            components->b = VK_COMPONENT_SWIZZLE_ZERO;
            components->a = VK_COMPONENT_SWIZZLE_ZERO;
        }
        else
        {
            FIXME("Stencil swizzle is not supported for format %#x.\n",
                    format->dxgi_format);
        }
    }

    if (format->dxgi_format == DXGI_FORMAT_A8_UNORM)
    {
        if (allowed_swizzle)
        {
            components->r = VK_COMPONENT_SWIZZLE_ZERO;
            components->g = VK_COMPONENT_SWIZZLE_ZERO;
            components->b = VK_COMPONENT_SWIZZLE_ZERO;
            components->a = VK_COMPONENT_SWIZZLE_R;
        }
        else
        {
            FIXME("Alpha swizzle is not supported.\n");
        }
    }

    if (format->dxgi_format == DXGI_FORMAT_B8G8R8X8_UNORM
            || format->dxgi_format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB)
    {
        if (allowed_swizzle)
        {
            components->r = VK_COMPONENT_SWIZZLE_R;
            components->g = VK_COMPONENT_SWIZZLE_G;
            components->b = VK_COMPONENT_SWIZZLE_B;
            components->a = VK_COMPONENT_SWIZZLE_ONE;
        }
        else
        {
            FIXME("B8G8R8X8 swizzle is not supported.\n");
        }
    }
}

static VkComponentSwizzle vk_component_swizzle_from_d3d12(unsigned int component_mapping,
        unsigned int component_index)
{
    D3D12_SHADER_COMPONENT_MAPPING mapping
            = D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(component_index, component_mapping);

    switch (mapping)
    {
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0:
            return VK_COMPONENT_SWIZZLE_R;
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1:
            return VK_COMPONENT_SWIZZLE_G;
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2:
            return VK_COMPONENT_SWIZZLE_B;
        case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3:
            return VK_COMPONENT_SWIZZLE_A;
        case D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0:
            return VK_COMPONENT_SWIZZLE_ZERO;
        case D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1:
            return VK_COMPONENT_SWIZZLE_ONE;
    }

    FIXME("Invalid component mapping %#x.\n", mapping);
    return VK_COMPONENT_SWIZZLE_IDENTITY;
}

static void vk_component_mapping_from_d3d12(VkComponentMapping *components,
        unsigned int component_mapping)
{
    components->r = vk_component_swizzle_from_d3d12(component_mapping, 0);
    components->g = vk_component_swizzle_from_d3d12(component_mapping, 1);
    components->b = vk_component_swizzle_from_d3d12(component_mapping, 2);
    components->a = vk_component_swizzle_from_d3d12(component_mapping, 3);
}

static VkComponentSwizzle swizzle_vk_component(const VkComponentMapping *components,
        VkComponentSwizzle component, VkComponentSwizzle swizzle)
{
    switch (swizzle)
    {
        case VK_COMPONENT_SWIZZLE_IDENTITY:
            break;

        case VK_COMPONENT_SWIZZLE_R:
            component = components->r;
            break;

        case VK_COMPONENT_SWIZZLE_G:
            component = components->g;
            break;

        case VK_COMPONENT_SWIZZLE_B:
            component = components->b;
            break;

        case VK_COMPONENT_SWIZZLE_A:
            component = components->a;
            break;

        case VK_COMPONENT_SWIZZLE_ONE:
        case VK_COMPONENT_SWIZZLE_ZERO:
            component = swizzle;
            break;

        default:
            FIXME("Invalid component swizzle %#x.\n", swizzle);
            break;
    }

    assert(component != VK_COMPONENT_SWIZZLE_IDENTITY);
    return component;
}

static void vk_component_mapping_compose(VkComponentMapping *dst, const VkComponentMapping *b)
{
    const VkComponentMapping a = *dst;

    dst->r = swizzle_vk_component(&a, a.r, b->r);
    dst->g = swizzle_vk_component(&a, a.g, b->g);
    dst->b = swizzle_vk_component(&a, a.b, b->b);
    dst->a = swizzle_vk_component(&a, a.a, b->a);
}

static bool init_default_texture_view_desc(struct vkd3d_texture_view_desc *desc,
        struct d3d12_resource *resource, DXGI_FORMAT view_format)
{
    const struct d3d12_device *device = resource->device;

    if (!(desc->format = vkd3d_format_from_d3d12_resource_desc(device, &resource->desc, view_format)))
    {
        FIXME("Failed to find format (resource format %#x, view format %#x).\n",
                resource->desc.Format, view_format);
        return false;
    }

    desc->aspect_mask = desc->format->vk_aspect_mask;
    desc->image = resource->res.vk_image;
    desc->miplevel_idx = 0;
    desc->miplevel_count = 1;
    desc->miplevel_clamp = 0.0f;
    desc->layer_idx = 0;
    desc->layer_count = d3d12_resource_desc_get_layer_count(&resource->desc);

    switch (resource->desc.Dimension)
    {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            desc->view_type = resource->desc.DepthOrArraySize > 1
                    ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            desc->view_type = resource->desc.DepthOrArraySize > 1
                    ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            desc->view_type = VK_IMAGE_VIEW_TYPE_3D;
            desc->layer_count = 1;
            break;

        default:
            FIXME("Resource dimension %#x not implemented.\n", resource->desc.Dimension);
            return false;
    }

    desc->components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    desc->components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    desc->components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    desc->components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    desc->allowed_swizzle = false;
    return true;
}

bool vkd3d_create_texture_view(struct d3d12_device *device, const struct vkd3d_texture_view_desc *desc, struct vkd3d_view **view)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    const struct vkd3d_format *format = desc->format;
    struct VkImageViewCreateInfo view_desc;
    struct vkd3d_view *object;
    VkImageView vk_view;
    VkResult vr;

    view_desc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_desc.pNext = NULL;
    view_desc.flags = 0;
    view_desc.image = desc->image;
    view_desc.viewType = desc->view_type;
    view_desc.format = format->vk_format;
    vkd3d_set_view_swizzle_for_format(&view_desc.components, format, desc->allowed_swizzle);
    if (desc->allowed_swizzle)
        vk_component_mapping_compose(&view_desc.components, &desc->components);
    view_desc.subresourceRange.aspectMask = desc->aspect_mask;
    view_desc.subresourceRange.baseMipLevel = desc->miplevel_idx;
    view_desc.subresourceRange.levelCount = desc->miplevel_count;
    view_desc.subresourceRange.baseArrayLayer = desc->layer_idx;
    view_desc.subresourceRange.layerCount = desc->layer_count;

    if (desc->miplevel_clamp != 0.0f)
        FIXME_ONCE("Cannot handle MinResourceLOD clamp of %f correctly.\n", desc->miplevel_clamp);

    /* This is not correct, but it's the best we can do with existing API.
     * It should at least avoid a scenario where implicit LOD fetches from invalid levels.
     * TODO: We will need an extension with vkCreateImageView pNext specifying minLODClamp.
     * It will be trivial to add in RADV at least ... */
    if (desc->miplevel_clamp >= 1.0f)
    {
        uint32_t clamp_base_level;
        uint32_t new_base_level;
        uint32_t end_level;

        clamp_base_level = max((uint32_t)desc->miplevel_clamp, view_desc.subresourceRange.baseMipLevel);
        if (view_desc.subresourceRange.levelCount != VK_REMAINING_MIP_LEVELS)
        {
            end_level = view_desc.subresourceRange.baseMipLevel + view_desc.subresourceRange.levelCount;
            new_base_level = min(end_level - 1, clamp_base_level);
            view_desc.subresourceRange.levelCount = end_level - new_base_level;
            view_desc.subresourceRange.baseMipLevel = new_base_level;
        }
        else
            view_desc.subresourceRange.baseMipLevel = clamp_base_level;
    }

    if ((vr = VK_CALL(vkCreateImageView(device->vk_device, &view_desc, NULL, &vk_view))) < 0)
    {
        WARN("Failed to create Vulkan image view, vr %d.\n", vr);
        return false;
    }

    if (!(object = vkd3d_view_create(VKD3D_VIEW_TYPE_IMAGE)))
    {
        VK_CALL(vkDestroyImageView(device->vk_device, vk_view, NULL));
        return false;
    }

    object->vk_image_view = vk_view;
    object->format = format;
    object->info.texture.vk_view_type = desc->view_type;
    object->info.texture.miplevel_idx = desc->miplevel_idx;
    object->info.texture.layer_idx = desc->layer_idx;
    object->info.texture.layer_count = desc->layer_count;
    *view = object;
    return true;
}

static inline void vkd3d_init_write_descriptor_set(VkWriteDescriptorSet *vk_write, const struct d3d12_desc *descriptor,
        struct vkd3d_descriptor_binding binding,
        VkDescriptorType vk_descriptor_type, const union vkd3d_descriptor_info *info)
{
    vk_write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    vk_write->pNext = NULL;
    vk_write->dstSet = descriptor->heap->vk_descriptor_sets[binding.set];
    vk_write->dstBinding = binding.binding;
    vk_write->dstArrayElement = d3d12_desc_heap_offset(descriptor);
    vk_write->descriptorCount = 1;
    vk_write->descriptorType = vk_descriptor_type;
    vk_write->pImageInfo = &info->image;
    vk_write->pBufferInfo = &info->buffer;
    vk_write->pTexelBufferView = &info->buffer_view;
}

static void d3d12_descriptor_heap_write_null_descriptor_template(struct d3d12_desc *desc,
        VkDescriptorType vk_mutable_descriptor_type)
{
    /* For null descriptors, some games don't write the correct type (usually an image SRV),
     * so we will need to splat null descriptors over all descriptor sets.
     * For MUTABLE, this would normally just be one descriptor set, but
     * we need MUTABLE + STORAGE_BUFFER, or 6 sets for non-mutable :\ */
    VkWriteDescriptorSet writes[ARRAY_SIZE(desc->heap->null_descriptor_template.writes)];
    const struct vkd3d_vk_device_procs *vk_procs;
    struct d3d12_descriptor_heap *heap;
    unsigned int num_writes, i;
    unsigned int offset;
    VkDeviceAddress *va;

    heap = desc->heap;

    /* When mutable descriptors are not supported, set a dummy type.
       This will make those drivers not care about the null type being different between
       null writes. */
    if (!heap->null_descriptor_template.has_mutable_descriptors)
        vk_mutable_descriptor_type = 0;

    /* Skip writes with the same null type that are already null. */
    if (!(desc->metadata.flags & VKD3D_DESCRIPTOR_FLAG_NON_NULL)
            && desc->metadata.current_null_type == vk_mutable_descriptor_type)
        return;

    num_writes = heap->null_descriptor_template.num_writes;
    vk_procs = &heap->device->vk_procs;
    offset = desc->heap_offset;

    for (i = 0; i < num_writes; i++)
    {
        writes[i] = heap->null_descriptor_template.writes[i];
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_MUTABLE_VALVE)
            writes[i].descriptorType = vk_mutable_descriptor_type;
        writes[i].dstArrayElement = offset;
    }

    VK_CALL(vkUpdateDescriptorSets(heap->device->vk_device, num_writes, writes, 0, NULL));

    desc->metadata.cookie = 0;
    desc->metadata.flags = 0;
    desc->metadata.set_info_mask = heap->null_descriptor_template.set_info_mask;
    desc->metadata.current_null_type = vk_mutable_descriptor_type;
    memset(&desc->info, 0, sizeof(desc->info));

    va = heap->raw_va_aux_buffer.host_ptr;
    if (va)
        va[offset] = 0;

    /* Notify descriptor QA that we have a universal null descriptor. */
    vkd3d_descriptor_debug_write_descriptor(heap->descriptor_heap_info.host_ptr,
            heap->cookie, offset,
            VKD3D_DESCRIPTOR_QA_TYPE_UNIFORM_BUFFER_BIT |
                    VKD3D_DESCRIPTOR_QA_TYPE_STORAGE_BUFFER_BIT |
                    VKD3D_DESCRIPTOR_QA_TYPE_SAMPLED_IMAGE_BIT |
                    VKD3D_DESCRIPTOR_QA_TYPE_STORAGE_IMAGE_BIT |
                    VKD3D_DESCRIPTOR_QA_TYPE_UNIFORM_TEXEL_BUFFER_BIT |
                    VKD3D_DESCRIPTOR_QA_TYPE_STORAGE_TEXEL_BUFFER_BIT |
                    VKD3D_DESCRIPTOR_QA_TYPE_RAW_VA_BIT |
                    VKD3D_DESCRIPTOR_QA_TYPE_RT_ACCELERATION_STRUCTURE_BIT, 0);
}

void d3d12_desc_create_cbv(struct d3d12_desc *descriptor,
        struct d3d12_device *device, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    const struct vkd3d_unique_resource *resource = NULL;
    union vkd3d_descriptor_info descriptor_info;
    VkDescriptorType vk_descriptor_type;
    VkWriteDescriptorSet vk_write;
    uint32_t info_index;

    if (!desc)
    {
        WARN("Constant buffer desc is NULL.\n");
        return;
    }

    if (desc->SizeInBytes & (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))
    {
        WARN("Size is not %u bytes aligned.\n", D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        return;
    }

    vk_descriptor_type = vkd3d_bindless_state_get_cbv_descriptor_type(&device->bindless_state);

    if (!desc->BufferLocation)
    {
        d3d12_descriptor_heap_write_null_descriptor_template(descriptor, vk_descriptor_type);
        return;
    }

    resource = vkd3d_va_map_deref(&device->memory_allocator.va_map, desc->BufferLocation);
    descriptor_info.buffer.buffer = resource->vk_buffer;
    descriptor_info.buffer.offset = desc->BufferLocation - resource->va;
    descriptor_info.buffer.range = min(desc->SizeInBytes, resource->size - descriptor_info.buffer.offset);

    info_index = vkd3d_bindless_state_find_set_info_index(&device->bindless_state, VKD3D_BINDLESS_SET_CBV);

    descriptor->metadata.cookie = resource ? resource->cookie : 0;
    descriptor->metadata.set_info_mask = 1u << info_index;
    descriptor->metadata.flags = VKD3D_DESCRIPTOR_FLAG_OFFSET_RANGE | VKD3D_DESCRIPTOR_FLAG_NON_NULL;
    descriptor->info.buffer = descriptor_info.buffer;

    vkd3d_init_write_descriptor_set(&vk_write, descriptor,
            vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, info_index),
            vk_descriptor_type, &descriptor_info);

    vkd3d_descriptor_debug_write_descriptor(descriptor->heap->descriptor_heap_info.host_ptr,
            descriptor->heap->cookie,
            descriptor->heap_offset,
            vk_descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ?
                    VKD3D_DESCRIPTOR_QA_TYPE_UNIFORM_BUFFER_BIT :
                    VKD3D_DESCRIPTOR_QA_TYPE_STORAGE_BUFFER_BIT,
            descriptor->metadata.cookie);

    VK_CALL(vkUpdateDescriptorSets(device->vk_device, 1, &vk_write, 0, NULL));
}

static unsigned int vkd3d_view_flags_from_d3d12_buffer_srv_flags(D3D12_BUFFER_SRV_FLAGS flags)
{
    if (flags == D3D12_BUFFER_SRV_FLAG_RAW)
        return VKD3D_VIEW_RAW_BUFFER;
    if (flags)
        FIXME("Unhandled buffer SRV flags %#x.\n", flags);
    return 0;
}

static void vkd3d_buffer_view_get_bound_range_ssbo(struct d3d12_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        VkDeviceSize offset, VkDeviceSize range, VkDescriptorBufferInfo *vk_buffer,
        struct vkd3d_bound_buffer_range *bound_range)
{
    if (resource)
    {
        VkDeviceSize alignment = d3d12_device_get_ssbo_alignment(device);
        VkDeviceSize aligned_begin = offset & ~(alignment - 1);
        VkDeviceSize aligned_end = min((offset + range + alignment - 1) & ~(alignment - 1), resource->desc.Width);

        /* heap_offset is guaranteed to have 64KiB alignment */
        vk_buffer->buffer = resource->res.vk_buffer;
        vk_buffer->offset = resource->mem.offset + aligned_begin;
        vk_buffer->range = aligned_end - aligned_begin;

        bound_range->byte_offset = offset - aligned_begin;
        bound_range->byte_count = range;
    }
    else
    {
        vk_buffer->buffer = VK_NULL_HANDLE;
        vk_buffer->offset = 0;
        vk_buffer->range = VK_WHOLE_SIZE;

        bound_range->byte_offset = 0;
        bound_range->byte_count = 0;
    }
}

static bool vkd3d_buffer_view_get_aligned_view(struct d3d12_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        DXGI_FORMAT format, unsigned int vk_flags,
        VkDeviceSize first_element, VkDeviceSize num_elements,
        VkDeviceSize structured_stride, struct vkd3d_bound_buffer_range *bound_range,
        struct vkd3d_view **view)
{
    const struct vkd3d_format *vkd3d_format;
    VkDeviceSize max_resource_elements;
    VkDeviceSize max_element_headroom;
    VkDeviceSize element_align;
    VkDeviceSize max_elements;
    VkDeviceSize begin_range;
    VkDeviceSize end_range;

    if (device->bindless_state.flags & VKD3D_TYPED_OFFSET_BUFFER)
    {
        /* For typed buffers, we will try to remove two cases of extreme hashmap contention, i.e.
         * first_element and num_elements. By quantizing these two and relying on offset buffers,
         * we should achieve a bounded value for number of possible views we can create for a given resource. */
        max_elements = device->device_info.properties2.properties.limits.maxTexelBufferElements;

        if (format)
        {
            vkd3d_format = vkd3d_get_format(device, format, false);
            max_resource_elements = resource->desc.Width / vkd3d_format->byte_count;
        }
        else
        {
            /* For structured buffers, we need to rescale input parameters to
             * be in terms of u32 since the offset buffer must be in terms of words.
             * When using typed buffers, the offset buffer is in format of u32
             * (element offset, element size). */
            first_element = (first_element * structured_stride) / sizeof(uint32_t);
            num_elements = (num_elements * structured_stride) / sizeof(uint32_t);
            structured_stride = sizeof(uint32_t);
            max_resource_elements = resource->desc.Width / sizeof(uint32_t);
        }

        /* Requantizing the typed offset is shaky business if we overflow max_elements when doing so.
         * We can always fall back to 0 offset for the difficult and rare cases. */

        if (num_elements > max_elements)
        {
            FIXME("Application is attempting to use more elements in a typed buffer (%llu) than supported by device (%llu).\n",
                  (unsigned long long)num_elements, (unsigned long long)max_elements);
            bound_range->element_offset = 0;
            bound_range->element_count = num_elements;
        }
        else if (num_elements >= max_resource_elements)
        {
            bound_range->element_offset = 0;
            bound_range->element_count = num_elements;
        }
        else
        {
            /* Quantizing to alignment of N will at most increment number of elements in the view by N - 1. */
            max_element_headroom = max_elements - num_elements + 1;

            /* Based on headroom, align offset to the largest POT factor of N. */
            element_align = 1u << vkd3d_log2i(max_element_headroom);

            begin_range = first_element & ~(element_align - 1);
            end_range = (first_element + num_elements + element_align - 1) & ~(element_align - 1);
            end_range = min(end_range, max_resource_elements);

            bound_range->element_offset = first_element - begin_range;
            bound_range->element_count = num_elements;

            first_element = begin_range;
            num_elements = end_range - begin_range;
        }
    }

    if (!vkd3d_create_buffer_view_for_resource(device, resource, format,
            first_element, num_elements,
            structured_stride, vk_flags, view))
        return false;

    return true;
}

static void vkd3d_create_buffer_srv(struct d3d12_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VKD3D_UNUSED vkd3d_descriptor_qa_flags descriptor_qa_flags = 0;
    struct vkd3d_bound_buffer_range bound_range = { 0, 0, 0, 0 };
    union vkd3d_descriptor_info descriptor_info[2];
    VkDescriptorType vk_descriptor_type;
    VkWriteDescriptorSet vk_write[2];
    struct vkd3d_view *view = NULL;
    uint32_t vk_write_count = 0;
    unsigned int vk_flags;
    uint32_t info_index;

    if (!desc)
    {
        FIXME("Default buffer SRV not supported.\n");
        return;
    }

    if (desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
    {
        if (!desc->RaytracingAccelerationStructure.Location)
        {
            /* There is no concrete descriptor to use here,
             * so just write a SAMPLED_IMAGE to clear out mutable descriptor.
             * What we really want to clear here is the raw VA. */
            d3d12_descriptor_heap_write_null_descriptor_template(descriptor, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            return;
        }

        if (d3d12_device_supports_ray_tracing_tier_1_0(device))
        {
            /* We implement this as a raw VA in the aux buffer. */
            VkDeviceAddress *raw_addresses = descriptor->heap->raw_va_aux_buffer.host_ptr;
            uint32_t descriptor_index = d3d12_desc_heap_offset(descriptor);
            raw_addresses[descriptor_index] = desc->RaytracingAccelerationStructure.Location;
            descriptor->metadata.flags = VKD3D_DESCRIPTOR_FLAG_RAW_VA_AUX_BUFFER |
                                         VKD3D_DESCRIPTOR_FLAG_NON_NULL;
            descriptor->metadata.set_info_mask = 0;
            /* There is no resource tied to this descriptor, just a naked pointer. */
            descriptor->metadata.cookie = 0;
        }
        else
            WARN("Using CreateSRV for RTAS without RT support?\n");

        vkd3d_descriptor_debug_write_descriptor(descriptor->heap->descriptor_heap_info.host_ptr,
                descriptor->heap->cookie, descriptor->heap_offset,
                VKD3D_DESCRIPTOR_QA_TYPE_RT_ACCELERATION_STRUCTURE_BIT | VKD3D_DESCRIPTOR_QA_TYPE_RAW_VA_BIT,
                descriptor->metadata.cookie);

        return;
    }

    if (desc->ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
    {
        WARN("Unexpected view dimension %#x.\n", desc->ViewDimension);
        return;
    }

    if (!resource)
    {
        /* In the mutable set, always write texel buffer. The STORAGE_BUFFER set is also written to. */
        d3d12_descriptor_heap_write_null_descriptor_template(descriptor, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
        return;
    }

    descriptor->metadata.set_info_mask = 0;
    descriptor->metadata.flags = 0;

    if (d3d12_device_use_ssbo_raw_buffer(device))
    {
        VkDeviceSize stride = desc->Format == DXGI_FORMAT_UNKNOWN
                ? desc->Buffer.StructureByteStride :
                vkd3d_get_format(device, desc->Format, false)->byte_count;

        vkd3d_buffer_view_get_bound_range_ssbo(descriptor, device, resource,
                desc->Buffer.FirstElement * stride, desc->Buffer.NumElements * stride,
                &descriptor_info[vk_write_count].buffer, &bound_range);

        info_index = vkd3d_bindless_state_find_set_info_index(&device->bindless_state,
                VKD3D_BINDLESS_SET_SRV | VKD3D_BINDLESS_SET_RAW_SSBO);

        descriptor->info.buffer = descriptor_info[vk_write_count].buffer;
        descriptor->metadata.cookie = resource ? resource->res.cookie : 0;
        descriptor->metadata.set_info_mask |= 1u << info_index;

        descriptor->metadata.flags |= VKD3D_DESCRIPTOR_FLAG_OFFSET_RANGE |
                                      VKD3D_DESCRIPTOR_FLAG_NON_NULL;
        if (device->bindless_state.flags & VKD3D_SSBO_OFFSET_BUFFER)
            descriptor->metadata.flags |= VKD3D_DESCRIPTOR_FLAG_BUFFER_OFFSET;

        vk_descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_qa_flags |= VKD3D_DESCRIPTOR_QA_TYPE_STORAGE_BUFFER_BIT;
        vkd3d_init_write_descriptor_set(&vk_write[vk_write_count], descriptor,
                vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, info_index),
                vk_descriptor_type, &descriptor_info[vk_write_count]);
        vk_write_count++;
    }

    vk_flags = vkd3d_view_flags_from_d3d12_buffer_srv_flags(desc->Buffer.Flags);
    if (!vkd3d_buffer_view_get_aligned_view(descriptor, device, resource, desc->Format, vk_flags,
            desc->Buffer.FirstElement, desc->Buffer.NumElements, desc->Buffer.StructureByteStride,
            &bound_range, &view))
        return;

    descriptor_info[vk_write_count].buffer_view = view ? view->vk_buffer_view : VK_NULL_HANDLE;

    info_index = vkd3d_bindless_state_find_set_info_index(&device->bindless_state,
            VKD3D_BINDLESS_SET_SRV | VKD3D_BINDLESS_SET_BUFFER);

    descriptor->info.view = view;
    /* Typed cookie takes precedence over raw cookie.
     * The typed cookie is more unique than raw cookie,
     * since raw cookie is just the ID3D12Resource. */
    descriptor->metadata.cookie = view ? view->cookie : 0;
    descriptor->metadata.set_info_mask |= 1u << info_index;

    descriptor->metadata.flags |= VKD3D_DESCRIPTOR_FLAG_VIEW | VKD3D_DESCRIPTOR_FLAG_NON_NULL;
    if (device->bindless_state.flags & VKD3D_TYPED_OFFSET_BUFFER)
        descriptor->metadata.flags |= VKD3D_DESCRIPTOR_FLAG_BUFFER_OFFSET;

    vk_descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    descriptor_qa_flags |= VKD3D_DESCRIPTOR_QA_TYPE_UNIFORM_TEXEL_BUFFER_BIT;

    vkd3d_init_write_descriptor_set(&vk_write[vk_write_count], descriptor,
            vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, info_index),
            vk_descriptor_type, &descriptor_info[vk_write_count]);
    vk_write_count++;

    if (descriptor->metadata.flags & VKD3D_DESCRIPTOR_FLAG_BUFFER_OFFSET)
    {
        struct vkd3d_bound_buffer_range *buffer_ranges = descriptor->heap->buffer_ranges.host_ptr;
        buffer_ranges[descriptor->heap_offset] = bound_range;
    }

    vkd3d_descriptor_debug_write_descriptor(descriptor->heap->descriptor_heap_info.host_ptr,
            descriptor->heap->cookie, descriptor->heap_offset, descriptor_qa_flags, descriptor->metadata.cookie);

    if (vk_write_count)
        VK_CALL(vkUpdateDescriptorSets(device->vk_device, vk_write_count, vk_write, 0, NULL));
}

static void vkd3d_create_texture_srv(struct d3d12_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    union vkd3d_descriptor_info descriptor_info;
    struct vkd3d_view *view = NULL;
    VkWriteDescriptorSet vk_write;
    struct vkd3d_view_key key;
    uint32_t info_index;

    if (!resource)
    {
        d3d12_descriptor_heap_write_null_descriptor_template(descriptor, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        return;
    }

    if (!init_default_texture_view_desc(&key.u.texture, resource, desc ? desc->Format : 0))
        return;

    key.view_type = VKD3D_VIEW_TYPE_IMAGE;
    key.u.texture.miplevel_count = VK_REMAINING_MIP_LEVELS;
    key.u.texture.allowed_swizzle = true;

    if (desc)
    {
        if (desc->Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)
        {
            TRACE("Component mapping %s for format %#x.\n",
                    debug_d3d12_shader_component_mapping(desc->Shader4ComponentMapping), desc->Format);

            vk_component_mapping_from_d3d12(&key.u.texture.components, desc->Shader4ComponentMapping);
        }

        switch (desc->ViewDimension)
        {
            case D3D12_SRV_DIMENSION_TEXTURE1D:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_1D;
                key.u.texture.miplevel_idx = desc->Texture1D.MostDetailedMip;
                key.u.texture.miplevel_count = desc->Texture1D.MipLevels;
                key.u.texture.miplevel_clamp = desc->Texture1D.ResourceMinLODClamp;
                key.u.texture.layer_count = 1;
                break;
            case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
                key.u.texture.miplevel_idx = desc->Texture1DArray.MostDetailedMip;
                key.u.texture.miplevel_count = desc->Texture1DArray.MipLevels;
                key.u.texture.miplevel_clamp = desc->Texture1DArray.ResourceMinLODClamp;
                key.u.texture.layer_idx = desc->Texture1DArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture1DArray.ArraySize;
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2D:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D;
                key.u.texture.miplevel_idx = desc->Texture2D.MostDetailedMip;
                key.u.texture.miplevel_count = desc->Texture2D.MipLevels;
                key.u.texture.miplevel_clamp = desc->Texture2D.ResourceMinLODClamp;
                key.u.texture.layer_count = 1;
                key.u.texture.aspect_mask = vk_image_aspect_flags_from_d3d12(resource->format, desc->Texture2D.PlaneSlice);
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.u.texture.miplevel_idx = desc->Texture2DArray.MostDetailedMip;
                key.u.texture.miplevel_count = desc->Texture2DArray.MipLevels;
                key.u.texture.miplevel_clamp = desc->Texture2DArray.ResourceMinLODClamp;
                key.u.texture.layer_idx = desc->Texture2DArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture2DArray.ArraySize;
                key.u.texture.aspect_mask = vk_image_aspect_flags_from_d3d12(resource->format, desc->Texture2DArray.PlaneSlice);
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DMS:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D;
                key.u.texture.layer_count = 1;
                break;
            case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.u.texture.layer_idx = desc->Texture2DMSArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture2DMSArray.ArraySize;
                break;
            case D3D12_SRV_DIMENSION_TEXTURE3D:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_3D;
                key.u.texture.miplevel_idx = desc->Texture3D.MostDetailedMip;
                key.u.texture.miplevel_count = desc->Texture3D.MipLevels;
                key.u.texture.miplevel_clamp = desc->Texture3D.ResourceMinLODClamp;
                break;
            case D3D12_SRV_DIMENSION_TEXTURECUBE:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_CUBE;
                key.u.texture.miplevel_idx = desc->TextureCube.MostDetailedMip;
                key.u.texture.miplevel_count = desc->TextureCube.MipLevels;
                key.u.texture.miplevel_clamp = desc->TextureCube.ResourceMinLODClamp;
                key.u.texture.layer_count = 6;
                break;
            case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                key.u.texture.miplevel_idx = desc->TextureCubeArray.MostDetailedMip;
                key.u.texture.miplevel_count = desc->TextureCubeArray.MipLevels;
                key.u.texture.miplevel_clamp = desc->TextureCubeArray.ResourceMinLODClamp;
                key.u.texture.layer_idx = desc->TextureCubeArray.First2DArrayFace;
                key.u.texture.layer_count = desc->TextureCubeArray.NumCubes;
                if (key.u.texture.layer_count != VK_REMAINING_ARRAY_LAYERS)
                    key.u.texture.layer_count *= 6;
                break;
            default:
                FIXME("Unhandled view dimension %#x.\n", desc->ViewDimension);
        }
    }

    /* Only applicable to workaround path. */
    key.u.texture.miplevel_clamp = min(key.u.texture.miplevel_clamp, (float)resource->desc.MipLevels - 1.0f);

    if (!(view = vkd3d_view_map_create_view(&resource->view_map, device, &key)))
        return;

    descriptor_info.image.sampler = VK_NULL_HANDLE;
    descriptor_info.image.imageView = view ? view->vk_image_view : VK_NULL_HANDLE;
    descriptor_info.image.imageLayout = view ? resource->common_layout : VK_IMAGE_LAYOUT_UNDEFINED;

    info_index = vkd3d_bindless_state_find_set_info_index(&device->bindless_state,
            VKD3D_BINDLESS_SET_SRV | VKD3D_BINDLESS_SET_IMAGE);

    descriptor->info.view = view;
    descriptor->metadata.cookie = view ? view->cookie : 0;
    descriptor->metadata.set_info_mask = 1u << info_index;
    descriptor->metadata.flags = VKD3D_DESCRIPTOR_FLAG_VIEW | VKD3D_DESCRIPTOR_FLAG_NON_NULL;

    vkd3d_init_write_descriptor_set(&vk_write, descriptor,
            vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, info_index),
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &descriptor_info);

    vkd3d_descriptor_debug_write_descriptor(descriptor->heap->descriptor_heap_info.host_ptr,
            descriptor->heap->cookie, descriptor->heap_offset,
            VKD3D_DESCRIPTOR_QA_TYPE_SAMPLED_IMAGE_BIT, descriptor->metadata.cookie);

    VK_CALL(vkUpdateDescriptorSets(device->vk_device, 1, &vk_write, 0, NULL));
}

void d3d12_desc_create_srv(struct d3d12_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
    bool is_buffer;

    if (resource)
    {
        is_buffer = d3d12_resource_is_buffer(resource);
    }
    else if (desc)
    {
        is_buffer = desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER ||
                desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    }
    else
    {
        WARN("Description required for NULL SRV.");
        return;
    }

    if (is_buffer)
        vkd3d_create_buffer_srv(descriptor, device, resource, desc);
    else
        vkd3d_create_texture_srv(descriptor, device, resource, desc);
}

static unsigned int vkd3d_view_flags_from_d3d12_buffer_uav_flags(D3D12_BUFFER_UAV_FLAGS flags)
{
    if (flags == D3D12_BUFFER_UAV_FLAG_RAW)
        return VKD3D_VIEW_RAW_BUFFER;
    if (flags)
        FIXME("Unhandled buffer UAV flags %#x.\n", flags);
    return 0;
}

VkDeviceAddress vkd3d_get_buffer_device_address(struct d3d12_device *device, VkBuffer vk_buffer)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    VkBufferDeviceAddressInfoKHR address_info;
    address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR;
    address_info.pNext = NULL;
    address_info.buffer = vk_buffer;

    return VK_CALL(vkGetBufferDeviceAddressKHR(device->vk_device, &address_info));
}

VkDeviceAddress vkd3d_get_acceleration_structure_device_address(struct d3d12_device *device,
        VkAccelerationStructureKHR vk_acceleration_structure)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

    VkAccelerationStructureDeviceAddressInfoKHR address_info;
    address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    address_info.pNext = NULL;
    address_info.accelerationStructure = vk_acceleration_structure;

    return VK_CALL(vkGetAccelerationStructureDeviceAddressKHR(device->vk_device, &address_info));
}

static void vkd3d_create_buffer_uav(struct d3d12_desc *descriptor, struct d3d12_device *device,
        struct d3d12_resource *resource, struct d3d12_resource *counter_resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VKD3D_UNUSED vkd3d_descriptor_qa_flags descriptor_qa_flags = 0;
    struct vkd3d_bound_buffer_range bound_range = { 0, 0, 0, 0 };
    union vkd3d_descriptor_info descriptor_info[3];
    unsigned int flags, vk_write_count = 0;
    VkDescriptorType vk_descriptor_type;
    VkDeviceAddress uav_counter_address;
    VkWriteDescriptorSet vk_write[3];
    struct vkd3d_view *view = NULL;
    VkBufferView uav_counter_view;
    uint32_t info_index;

    if (!desc)
    {
        FIXME("Default buffer UAV not supported.\n");
        return;
    }

    if (desc->ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
    {
        WARN("Unexpected view dimension %#x.\n", desc->ViewDimension);
        return;
    }

    if (!resource)
    {
        /* In the mutable set, always write texel buffer. The STORAGE_BUFFER set is also written to. */
        d3d12_descriptor_heap_write_null_descriptor_template(descriptor, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
        return;
    }

    /* Handle UAV itself */
    flags = vkd3d_view_flags_from_d3d12_buffer_uav_flags(desc->Buffer.Flags);

    descriptor->metadata.set_info_mask = 0;
    descriptor->metadata.flags = VKD3D_DESCRIPTOR_FLAG_RAW_VA_AUX_BUFFER |
                                 VKD3D_DESCRIPTOR_FLAG_NON_NULL;

    if (d3d12_device_use_ssbo_raw_buffer(device))
    {
        VkDescriptorBufferInfo *buffer_info = &descriptor_info[vk_write_count].buffer;

        VkDeviceSize stride = desc->Format == DXGI_FORMAT_UNKNOWN
                ? desc->Buffer.StructureByteStride :
                vkd3d_get_format(device, desc->Format, false)->byte_count;

        vkd3d_buffer_view_get_bound_range_ssbo(descriptor, device, resource,
                desc->Buffer.FirstElement * stride, desc->Buffer.NumElements * stride,
                buffer_info, &bound_range);

        info_index = vkd3d_bindless_state_find_set_info_index(&device->bindless_state,
                VKD3D_BINDLESS_SET_UAV | VKD3D_BINDLESS_SET_RAW_SSBO);

        descriptor->info.buffer = *buffer_info;
        descriptor->metadata.cookie = resource ? resource->res.cookie : 0;
        descriptor->metadata.set_info_mask |= 1u << info_index;

        descriptor->metadata.flags |= VKD3D_DESCRIPTOR_FLAG_OFFSET_RANGE;
        if (device->bindless_state.flags & VKD3D_SSBO_OFFSET_BUFFER)
            descriptor->metadata.flags |= VKD3D_DESCRIPTOR_FLAG_BUFFER_OFFSET;

        vk_descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor_qa_flags |= VKD3D_DESCRIPTOR_QA_TYPE_STORAGE_BUFFER_BIT;

        vkd3d_init_write_descriptor_set(&vk_write[vk_write_count], descriptor,
                vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, info_index),
                vk_descriptor_type, &descriptor_info[vk_write_count]);
        vk_write_count++;
    }

    if (resource)
    {
        if (!vkd3d_buffer_view_get_aligned_view(descriptor, device, resource, desc->Format, flags,
                desc->Buffer.FirstElement, desc->Buffer.NumElements,
                desc->Buffer.StructureByteStride, &bound_range, &view))
            return;
    }

    info_index = vkd3d_bindless_state_find_set_info_index(&device->bindless_state,
            VKD3D_BINDLESS_SET_UAV | VKD3D_BINDLESS_SET_BUFFER);

    descriptor->info.view = view;
    /* Typed cookie takes precedence over raw cookie.
     * The typed cookie is more unique than raw cookie,
     * since raw cookie is just the ID3D12Resource. */
    descriptor->metadata.cookie = view ? view->cookie : 0;
    descriptor->metadata.set_info_mask |= 1u << info_index;

    descriptor->metadata.flags |= VKD3D_DESCRIPTOR_FLAG_VIEW;
    if (device->bindless_state.flags & VKD3D_TYPED_OFFSET_BUFFER)
        descriptor->metadata.flags |= VKD3D_DESCRIPTOR_FLAG_BUFFER_OFFSET;

    descriptor_info[vk_write_count].buffer_view = view ? view->vk_buffer_view : VK_NULL_HANDLE;

    vk_descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    descriptor_qa_flags |= VKD3D_DESCRIPTOR_QA_TYPE_STORAGE_TEXEL_BUFFER_BIT;

    vkd3d_init_write_descriptor_set(&vk_write[vk_write_count], descriptor,
            vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, info_index),
            vk_descriptor_type, &descriptor_info[vk_write_count]);
    vk_write_count++;

    if (descriptor->metadata.flags & VKD3D_DESCRIPTOR_FLAG_BUFFER_OFFSET)
    {
        struct vkd3d_bound_buffer_range *buffer_ranges = descriptor->heap->buffer_ranges.host_ptr;
        buffer_ranges[descriptor->heap_offset] = bound_range;
    }

    /* Handle UAV counter */
    uav_counter_view = VK_NULL_HANDLE;
    uav_counter_address = 0;

    if (resource && counter_resource)
    {
        assert(d3d12_resource_is_buffer(counter_resource));
        assert(desc->Buffer.StructureByteStride);

        if (device->bindless_state.flags & VKD3D_RAW_VA_AUX_BUFFER)
        {
            VkDeviceAddress address = vkd3d_get_buffer_device_address(device, counter_resource->res.vk_buffer);
            uav_counter_address = address + counter_resource->mem.offset + desc->Buffer.CounterOffsetInBytes;
        }
        else
        {
            struct vkd3d_view *view;

            if (!vkd3d_create_buffer_view_for_resource(device, counter_resource, DXGI_FORMAT_R32_UINT,
                    desc->Buffer.CounterOffsetInBytes / sizeof(uint32_t), 1, 0, 0, &view))
                return;

            uav_counter_view = view->vk_buffer_view;
        }

        /* This is used to denote that a counter descriptor is present, irrespective of underlying descriptor type. */
        descriptor_qa_flags |= VKD3D_DESCRIPTOR_QA_TYPE_RAW_VA_BIT;
    }

    if (device->bindless_state.flags & VKD3D_RAW_VA_AUX_BUFFER)
    {
        VkDeviceAddress *counter_addresses = descriptor->heap->raw_va_aux_buffer.host_ptr;
        uint32_t descriptor_index = d3d12_desc_heap_offset(descriptor);
        counter_addresses[descriptor_index] = uav_counter_address;
    }
    else
    {
        struct vkd3d_descriptor_binding binding = vkd3d_bindless_state_find_set(
                &device->bindless_state, VKD3D_BINDLESS_SET_UAV | VKD3D_BINDLESS_SET_AUX_BUFFER);

        descriptor_info[vk_write_count].buffer_view = uav_counter_view;
        vkd3d_init_write_descriptor_set(&vk_write[vk_write_count], descriptor,
                binding,
                VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, &descriptor_info[vk_write_count]);
        vk_write_count++;
    }

    vkd3d_descriptor_debug_write_descriptor(descriptor->heap->descriptor_heap_info.host_ptr,
            descriptor->heap->cookie, descriptor->heap_offset,
            descriptor_qa_flags, descriptor->metadata.cookie);

    VK_CALL(vkUpdateDescriptorSets(device->vk_device, vk_write_count, vk_write, 0, NULL));
}

static void vkd3d_create_texture_uav(struct d3d12_desc *descriptor,
        struct d3d12_device *device, struct d3d12_resource *resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    union vkd3d_descriptor_info descriptor_info;
    struct vkd3d_view *view = NULL;
    VkWriteDescriptorSet vk_write;
    struct vkd3d_view_key key;
    uint32_t info_index;

    if (!resource)
    {
        d3d12_descriptor_heap_write_null_descriptor_template(descriptor, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        return;
    }

    key.view_type = VKD3D_VIEW_TYPE_IMAGE;

    if (!init_default_texture_view_desc(&key.u.texture, resource, desc ? desc->Format : 0))
        return;

    if (vkd3d_format_is_compressed(key.u.texture.format))
    {
        WARN("UAVs cannot be created for compressed formats.\n");
        return;
    }

    if (desc)
    {
        switch (desc->ViewDimension)
        {
            case D3D12_UAV_DIMENSION_TEXTURE1D:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_1D;
                key.u.texture.miplevel_idx = desc->Texture1D.MipSlice;
                key.u.texture.layer_count = 1;
                break;
            case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
                key.u.texture.miplevel_idx = desc->Texture1DArray.MipSlice;
                key.u.texture.layer_idx = desc->Texture1DArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture1DArray.ArraySize;
                break;
            case D3D12_UAV_DIMENSION_TEXTURE2D:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D;
                key.u.texture.miplevel_idx = desc->Texture2D.MipSlice;
                key.u.texture.layer_count = 1;
                key.u.texture.aspect_mask = vk_image_aspect_flags_from_d3d12(resource->format, desc->Texture2D.PlaneSlice);
                break;
            case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.u.texture.miplevel_idx = desc->Texture2DArray.MipSlice;
                key.u.texture.layer_idx = desc->Texture2DArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture2DArray.ArraySize;
                key.u.texture.aspect_mask = vk_image_aspect_flags_from_d3d12(resource->format, desc->Texture2DArray.PlaneSlice);
                break;
            case D3D12_UAV_DIMENSION_TEXTURE3D:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_3D;
                key.u.texture.miplevel_idx = desc->Texture3D.MipSlice;
                if (desc->Texture3D.FirstWSlice ||
                    ((desc->Texture3D.WSize != max(1u, (UINT)resource->desc.DepthOrArraySize >> desc->Texture3D.MipSlice)) &&
                        (desc->Texture3D.WSize != UINT_MAX)))
                {
                    FIXME("Unhandled depth view %u-%u.\n",
                          desc->Texture3D.FirstWSlice, desc->Texture3D.WSize);
                }
                break;
            default:
                FIXME("Unhandled view dimension %#x.\n", desc->ViewDimension);
        }
    }

    if (!(view = vkd3d_view_map_create_view(&resource->view_map, device, &key)))
        return;

    descriptor_info.image.sampler = VK_NULL_HANDLE;
    descriptor_info.image.imageView = view ? view->vk_image_view : VK_NULL_HANDLE;
    descriptor_info.image.imageLayout = view ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;

    info_index = vkd3d_bindless_state_find_set_info_index(&device->bindless_state,
            VKD3D_BINDLESS_SET_UAV | VKD3D_BINDLESS_SET_IMAGE);

    descriptor->info.view = view;
    descriptor->metadata.cookie = view ? view->cookie : 0;
    descriptor->metadata.set_info_mask = 1u << info_index;
    descriptor->metadata.flags = VKD3D_DESCRIPTOR_FLAG_VIEW | VKD3D_DESCRIPTOR_FLAG_NON_NULL;

    vkd3d_init_write_descriptor_set(&vk_write, descriptor,
            vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, info_index),
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &descriptor_info);

    vkd3d_descriptor_debug_write_descriptor(descriptor->heap->descriptor_heap_info.host_ptr,
            descriptor->heap->cookie, descriptor->heap_offset,
            VKD3D_DESCRIPTOR_QA_TYPE_STORAGE_IMAGE_BIT, descriptor->metadata.cookie);

    VK_CALL(vkUpdateDescriptorSets(device->vk_device, 1, &vk_write, 0, NULL));
}

void d3d12_desc_create_uav(struct d3d12_desc *descriptor, struct d3d12_device *device,
        struct d3d12_resource *resource, struct d3d12_resource *counter_resource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    bool is_buffer;

    if (resource)
    {
        is_buffer = d3d12_resource_is_buffer(resource);
    }
    else if (desc)
    {
        is_buffer = desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER;
    }
    else
    {
        WARN("Description required for NULL UAV.");
        return;
    }

    if (counter_resource && (!resource || !is_buffer))
        FIXME("Ignoring counter resource %p.\n", counter_resource);

    if (is_buffer)
        vkd3d_create_buffer_uav(descriptor, device, resource, counter_resource, desc);
    else
        vkd3d_create_texture_uav(descriptor, device, resource, desc);
}

bool vkd3d_create_raw_buffer_view(struct d3d12_device *device,
        D3D12_GPU_VIRTUAL_ADDRESS gpu_address, VkBufferView *vk_buffer_view)
{
    const struct vkd3d_unique_resource *resource;
    uint64_t range;
    uint64_t offset;

    resource = vkd3d_va_map_deref(&device->memory_allocator.va_map, gpu_address);
    assert(resource && resource->va && resource->size);

    offset = gpu_address - resource->va;
    range = min(resource->size - offset, device->vk_info.device_limits.maxStorageBufferRange);

    return vkd3d_create_raw_r32ui_vk_buffer_view(device, resource->vk_buffer,
            offset, range, vk_buffer_view);
}

/* samplers */
static VkFilter vk_filter_from_d3d12(D3D12_FILTER_TYPE type)
{
    switch (type)
    {
        case D3D12_FILTER_TYPE_POINT:
            return VK_FILTER_NEAREST;
        case D3D12_FILTER_TYPE_LINEAR:
            return VK_FILTER_LINEAR;
        default:
            FIXME("Unhandled filter type %#x.\n", type);
            return VK_FILTER_NEAREST;
    }
}

static VkSamplerMipmapMode vk_mipmap_mode_from_d3d12(D3D12_FILTER_TYPE type)
{
    switch (type)
    {
        case D3D12_FILTER_TYPE_POINT:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case D3D12_FILTER_TYPE_LINEAR:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        default:
            FIXME("Unhandled filter type %#x.\n", type);
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
}

static VkSamplerAddressMode vk_address_mode_from_d3d12(D3D12_TEXTURE_ADDRESS_MODE mode)
{
    switch (mode)
    {
        case D3D12_TEXTURE_ADDRESS_MODE_WRAP:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case D3D12_TEXTURE_ADDRESS_MODE_CLAMP:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case D3D12_TEXTURE_ADDRESS_MODE_BORDER:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE:
            return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default:
            FIXME("Unhandled address mode %#x.\n", mode);
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkSamplerReductionModeEXT vk_reduction_mode_from_d3d12(D3D12_FILTER_REDUCTION_TYPE mode)
{
    switch (mode)
    {
        case D3D12_FILTER_REDUCTION_TYPE_STANDARD:
        case D3D12_FILTER_REDUCTION_TYPE_COMPARISON:
            return VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT;
        case D3D12_FILTER_REDUCTION_TYPE_MINIMUM:
            return VK_SAMPLER_REDUCTION_MODE_MIN_EXT;
        case D3D12_FILTER_REDUCTION_TYPE_MAXIMUM:
            return VK_SAMPLER_REDUCTION_MODE_MAX_EXT;
        default:
            FIXME("Unhandled reduction mode %#x.\n", mode);
            return VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT;
    }
}

static bool d3d12_sampler_needs_border_color(D3D12_TEXTURE_ADDRESS_MODE u,
        D3D12_TEXTURE_ADDRESS_MODE v, D3D12_TEXTURE_ADDRESS_MODE w)
{
    return u == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
        v == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
        w == D3D12_TEXTURE_ADDRESS_MODE_BORDER;
}

static VkBorderColor vk_static_border_color_from_d3d12(D3D12_STATIC_BORDER_COLOR border_color)
{
    switch (border_color)
    {
        case D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK:
            return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        case D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK:
            return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        case D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE:
            return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        default:
            WARN("Unhandled static border color %u.\n", border_color);
            return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
}

static VkBorderColor vk_border_color_from_d3d12(struct d3d12_device *device, const float *border_color)
{
    unsigned int i;

    static const struct
    {
        float color[4];
        VkBorderColor vk_border_color;
    }
    border_colors[] = {
      { {0.0f, 0.0f, 0.0f, 0.0f}, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK },
      { {0.0f, 0.0f, 0.0f, 1.0f}, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK },
      { {1.0f, 1.0f, 1.0f, 1.0f}, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE },
    };

    for (i = 0; i < ARRAY_SIZE(border_colors); i++)
    {
        if (!memcmp(border_color, border_colors[i].color, sizeof(border_colors[i].color)))
            return border_colors[i].vk_border_color;
    }

    if (!device->device_info.custom_border_color_features.customBorderColorWithoutFormat)
    {
        FIXME("Unsupported border color (%f, %f, %f, %f).\n",
                border_color[0], border_color[1], border_color[2], border_color[3]);
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }

    return VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
}

HRESULT d3d12_create_static_sampler(struct d3d12_device *device,
        const D3D12_STATIC_SAMPLER_DESC *desc, VkSampler *vk_sampler)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkSamplerReductionModeCreateInfoEXT reduction_desc;
    VkSamplerCreateInfo sampler_desc;
    VkResult vr;

    reduction_desc.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT;
    reduction_desc.pNext = NULL;
    reduction_desc.reductionMode = vk_reduction_mode_from_d3d12(D3D12_DECODE_FILTER_REDUCTION(desc->Filter));

    sampler_desc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_desc.pNext = NULL;
    sampler_desc.flags = 0;
    sampler_desc.magFilter = vk_filter_from_d3d12(D3D12_DECODE_MAG_FILTER(desc->Filter));
    sampler_desc.minFilter = vk_filter_from_d3d12(D3D12_DECODE_MIN_FILTER(desc->Filter));
    sampler_desc.mipmapMode = vk_mipmap_mode_from_d3d12(D3D12_DECODE_MIP_FILTER(desc->Filter));
    sampler_desc.addressModeU = vk_address_mode_from_d3d12(desc->AddressU);
    sampler_desc.addressModeV = vk_address_mode_from_d3d12(desc->AddressV);
    sampler_desc.addressModeW = vk_address_mode_from_d3d12(desc->AddressW);
    sampler_desc.mipLodBias = desc->MipLODBias;
    sampler_desc.anisotropyEnable = D3D12_DECODE_IS_ANISOTROPIC_FILTER(desc->Filter);
    sampler_desc.maxAnisotropy = desc->MaxAnisotropy;
    sampler_desc.compareEnable = D3D12_DECODE_IS_COMPARISON_FILTER(desc->Filter);
    sampler_desc.compareOp = sampler_desc.compareEnable ? vk_compare_op_from_d3d12(desc->ComparisonFunc) : 0;
    sampler_desc.minLod = desc->MinLOD;
    sampler_desc.maxLod = desc->MaxLOD;
    sampler_desc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sampler_desc.unnormalizedCoordinates = VK_FALSE;

    if (d3d12_sampler_needs_border_color(desc->AddressU, desc->AddressV, desc->AddressW))
        sampler_desc.borderColor = vk_static_border_color_from_d3d12(desc->BorderColor);

    if (reduction_desc.reductionMode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT && device->vk_info.EXT_sampler_filter_minmax)
        vk_prepend_struct(&sampler_desc, &reduction_desc);

    if ((vr = VK_CALL(vkCreateSampler(device->vk_device, &sampler_desc, NULL, vk_sampler))) < 0)
        WARN("Failed to create Vulkan sampler, vr %d.\n", vr);

    return hresult_from_vk_result(vr);
}

static HRESULT d3d12_create_sampler(struct d3d12_device *device,
        const D3D12_SAMPLER_DESC *desc, VkSampler *vk_sampler)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkSamplerCustomBorderColorCreateInfoEXT border_color_info;
    VkSamplerReductionModeCreateInfoEXT reduction_desc;
    VkSamplerCreateInfo sampler_desc;
    VkResult vr;

    border_color_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
    border_color_info.pNext = NULL;
    memcpy(border_color_info.customBorderColor.float32, desc->BorderColor,
            sizeof(border_color_info.customBorderColor.float32));
    border_color_info.format = VK_FORMAT_UNDEFINED;

    reduction_desc.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT;
    reduction_desc.pNext = NULL;
    reduction_desc.reductionMode = vk_reduction_mode_from_d3d12(D3D12_DECODE_FILTER_REDUCTION(desc->Filter));

    sampler_desc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_desc.pNext = NULL;
    sampler_desc.flags = 0;
    sampler_desc.magFilter = vk_filter_from_d3d12(D3D12_DECODE_MAG_FILTER(desc->Filter));
    sampler_desc.minFilter = vk_filter_from_d3d12(D3D12_DECODE_MIN_FILTER(desc->Filter));
    sampler_desc.mipmapMode = vk_mipmap_mode_from_d3d12(D3D12_DECODE_MIP_FILTER(desc->Filter));
    sampler_desc.addressModeU = vk_address_mode_from_d3d12(desc->AddressU);
    sampler_desc.addressModeV = vk_address_mode_from_d3d12(desc->AddressV);
    sampler_desc.addressModeW = vk_address_mode_from_d3d12(desc->AddressW);
    sampler_desc.mipLodBias = desc->MipLODBias;
    sampler_desc.anisotropyEnable = D3D12_DECODE_IS_ANISOTROPIC_FILTER(desc->Filter);
    sampler_desc.maxAnisotropy = desc->MaxAnisotropy;
    sampler_desc.compareEnable = D3D12_DECODE_IS_COMPARISON_FILTER(desc->Filter);
    sampler_desc.compareOp = sampler_desc.compareEnable ? vk_compare_op_from_d3d12(desc->ComparisonFunc) : 0;
    sampler_desc.minLod = desc->MinLOD;
    sampler_desc.maxLod = desc->MaxLOD;
    sampler_desc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sampler_desc.unnormalizedCoordinates = VK_FALSE;

    if (d3d12_sampler_needs_border_color(desc->AddressU, desc->AddressV, desc->AddressW))
        sampler_desc.borderColor = vk_border_color_from_d3d12(device, desc->BorderColor);

    if (sampler_desc.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT)
        vk_prepend_struct(&sampler_desc, &border_color_info);

    if (reduction_desc.reductionMode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT && device->vk_info.EXT_sampler_filter_minmax)
        vk_prepend_struct(&sampler_desc, &reduction_desc);

    if ((vr = VK_CALL(vkCreateSampler(device->vk_device, &sampler_desc, NULL, vk_sampler))) < 0)
        WARN("Failed to create Vulkan sampler, vr %d.\n", vr);

    return hresult_from_vk_result(vr);
}

void d3d12_desc_create_sampler(struct d3d12_desc *sampler,
        struct d3d12_device *device, const D3D12_SAMPLER_DESC *desc)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    union vkd3d_descriptor_info descriptor_info;
    VkWriteDescriptorSet vk_write;
    struct vkd3d_view_key key;
    struct vkd3d_view *view;
    uint32_t info_index;

    if (!desc)
    {
        WARN("NULL sampler desc.\n");
        return;
    }

    key.view_type = VKD3D_VIEW_TYPE_SAMPLER;
    key.u.sampler = *desc;

    if (!(view = vkd3d_view_map_create_view(&device->sampler_map, device, &key)))
        return;

    vkd3d_descriptor_debug_register_view_cookie(device->descriptor_qa_global_info, view->cookie, 0);

    info_index = vkd3d_bindless_state_find_set_info_index(&device->bindless_state, VKD3D_BINDLESS_SET_SAMPLER);

    sampler->info.view = view;
    sampler->metadata.cookie = view->cookie;
    sampler->metadata.set_info_mask = 1u << info_index;
    sampler->metadata.flags = VKD3D_DESCRIPTOR_FLAG_VIEW | VKD3D_DESCRIPTOR_FLAG_NON_NULL;

    descriptor_info.image.sampler = view->vk_sampler;
    descriptor_info.image.imageView = VK_NULL_HANDLE;
    descriptor_info.image.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vkd3d_init_write_descriptor_set(&vk_write, sampler,
            vkd3d_bindless_state_binding_from_info_index(&device->bindless_state, info_index),
            VK_DESCRIPTOR_TYPE_SAMPLER, &descriptor_info);

    vkd3d_descriptor_debug_write_descriptor(sampler->heap->descriptor_heap_info.host_ptr,
            sampler->heap->cookie, sampler->heap_offset,
            VKD3D_DESCRIPTOR_QA_TYPE_SAMPLER_BIT, sampler->metadata.cookie);

    VK_CALL(vkUpdateDescriptorSets(device->vk_device, 1, &vk_write, 0, NULL));
}

/* RTVs */
void d3d12_rtv_desc_copy(struct d3d12_rtv_desc *dst, struct d3d12_rtv_desc *src, unsigned int count)
{
    memcpy(dst, src, sizeof(*dst) * count);
}

void d3d12_rtv_desc_create_rtv(struct d3d12_rtv_desc *rtv_desc, struct d3d12_device *device,
        struct d3d12_resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc)
{
    struct vkd3d_view_key key;
    struct vkd3d_view *view;

    if (!resource)
    {
        memset(rtv_desc, 0, sizeof(*rtv_desc));
        return;
    }

    if (!(resource->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
        FIXME("Resource %p does not set D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET.\n", resource);

    if (!init_default_texture_view_desc(&key.u.texture, resource, desc ? desc->Format : 0))
        return;

    if (key.u.texture.format->vk_aspect_mask != VK_IMAGE_ASPECT_COLOR_BIT)
    {
        WARN("Trying to create RTV for depth/stencil format %#x.\n", key.u.texture.format->dxgi_format);
        return;
    }

    key.view_type = VKD3D_VIEW_TYPE_IMAGE;

    if (desc)
    {
        switch (desc->ViewDimension)
        {
            case D3D12_RTV_DIMENSION_TEXTURE1D:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_1D;
                key.u.texture.miplevel_idx = desc->Texture1D.MipSlice;
                key.u.texture.layer_count = 1;
                break;
            case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
                key.u.texture.miplevel_idx = desc->Texture1DArray.MipSlice;
                key.u.texture.layer_idx = desc->Texture1DArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture1DArray.ArraySize;
                break;
            case D3D12_RTV_DIMENSION_TEXTURE2D:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D;
                key.u.texture.miplevel_idx = desc->Texture2D.MipSlice;
                key.u.texture.layer_count = 1;
                key.u.texture.aspect_mask = vk_image_aspect_flags_from_d3d12(resource->format, desc->Texture2D.PlaneSlice);
                break;
            case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.u.texture.miplevel_idx = desc->Texture2DArray.MipSlice;
                key.u.texture.layer_idx = desc->Texture2DArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture2DArray.ArraySize;
                key.u.texture.aspect_mask = vk_image_aspect_flags_from_d3d12(resource->format, desc->Texture2DArray.PlaneSlice);
                break;
            case D3D12_RTV_DIMENSION_TEXTURE2DMS:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D;
                key.u.texture.layer_count = 1;
                break;
            case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.u.texture.layer_idx = desc->Texture2DMSArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture2DMSArray.ArraySize;
                break;
            case D3D12_RTV_DIMENSION_TEXTURE3D:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.u.texture.miplevel_idx = desc->Texture3D.MipSlice;
                key.u.texture.layer_idx = desc->Texture3D.FirstWSlice;
                key.u.texture.layer_count = desc->Texture3D.WSize;
                break;
            default:
                FIXME("Unhandled view dimension %#x.\n", desc->ViewDimension);
        }

        /* Avoid passing down UINT32_MAX here since that makes framebuffer logic later rather awkward. */
        key.u.texture.layer_count = min(key.u.texture.layer_count, resource->desc.DepthOrArraySize - key.u.texture.layer_idx);
    }
    else if (resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    {
        key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        key.u.texture.layer_idx = 0;
        key.u.texture.layer_count = resource->desc.DepthOrArraySize;
    }

    assert(d3d12_resource_is_texture(resource));

    if (!(view = vkd3d_view_map_create_view(&resource->view_map, device, &key)))
        return;

    vkd3d_descriptor_debug_register_view_cookie(device->descriptor_qa_global_info, view->cookie, resource->res.cookie);

    rtv_desc->sample_count = vk_samples_from_dxgi_sample_desc(&resource->desc.SampleDesc);
    rtv_desc->format = key.u.texture.format;
    rtv_desc->width = d3d12_resource_desc_get_width(&resource->desc, key.u.texture.miplevel_idx);
    rtv_desc->height = d3d12_resource_desc_get_height(&resource->desc, key.u.texture.miplevel_idx);
    rtv_desc->layer_count = key.u.texture.layer_count;
    rtv_desc->view = view;
    rtv_desc->resource = resource;
}

void d3d12_rtv_desc_create_dsv(struct d3d12_rtv_desc *dsv_desc, struct d3d12_device *device,
        struct d3d12_resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc)
{
    struct vkd3d_view_key key;
    struct vkd3d_view *view;

    if (!resource)
    {
        memset(dsv_desc, 0, sizeof(*dsv_desc));
        return;
    }

    if (!(resource->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        FIXME("Resource %p does not set D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL.\n", resource);

    if (resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    {
        WARN("Cannot create DSV for 3D texture.\n");
        return;
    }

    if (!init_default_texture_view_desc(&key.u.texture, resource, desc ? desc->Format : 0))
        return;

    if (!(key.u.texture.format->vk_aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)))
    {
        WARN("Trying to create DSV for format %#x.\n", key.u.texture.format->dxgi_format);
        return;
    }

    key.view_type = VKD3D_VIEW_TYPE_IMAGE;

    if (desc)
    {
        switch (desc->ViewDimension)
        {
            case D3D12_DSV_DIMENSION_TEXTURE1D:
                key.u.texture.miplevel_idx = desc->Texture1D.MipSlice;
                key.u.texture.layer_count = 1;
                break;
            case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
                key.u.texture.miplevel_idx = desc->Texture1DArray.MipSlice;
                key.u.texture.layer_idx = desc->Texture1DArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture1DArray.ArraySize;
                break;
            case D3D12_DSV_DIMENSION_TEXTURE2D:
                key.u.texture.miplevel_idx = desc->Texture2D.MipSlice;
                key.u.texture.layer_count = 1;
                break;
            case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.u.texture.miplevel_idx = desc->Texture2DArray.MipSlice;
                key.u.texture.layer_idx = desc->Texture2DArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture2DArray.ArraySize;
                break;
            case D3D12_DSV_DIMENSION_TEXTURE2DMS:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D;
                key.u.texture.layer_count = 1;
                break;
            case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
                key.u.texture.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                key.u.texture.layer_idx = desc->Texture2DMSArray.FirstArraySlice;
                key.u.texture.layer_count = desc->Texture2DMSArray.ArraySize;
                break;
            default:
                FIXME("Unhandled view dimension %#x.\n", desc->ViewDimension);
        }

        /* Avoid passing down UINT32_MAX here since that makes framebuffer logic later rather awkward. */
        key.u.texture.layer_count = min(key.u.texture.layer_count, resource->desc.DepthOrArraySize - key.u.texture.layer_idx);
    }

    assert(d3d12_resource_is_texture(resource));

    if (!(view = vkd3d_view_map_create_view(&resource->view_map, device, &key)))
        return;

    vkd3d_descriptor_debug_register_view_cookie(device->descriptor_qa_global_info, view->cookie, resource->res.cookie);

    dsv_desc->sample_count = vk_samples_from_dxgi_sample_desc(&resource->desc.SampleDesc);
    dsv_desc->format = key.u.texture.format;
    dsv_desc->width = d3d12_resource_desc_get_width(&resource->desc, key.u.texture.miplevel_idx);
    dsv_desc->height = d3d12_resource_desc_get_height(&resource->desc, key.u.texture.miplevel_idx);
    dsv_desc->layer_count = key.u.texture.layer_count;
    dsv_desc->view = view;
    dsv_desc->resource = resource;
}

/* ID3D12DescriptorHeap */
static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_QueryInterface(ID3D12DescriptorHeap *iface,
        REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D12DescriptorHeap)
            || IsEqualGUID(riid, &IID_ID3D12Pageable)
            || IsEqualGUID(riid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(riid, &IID_ID3D12Object)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D12DescriptorHeap_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_descriptor_heap_AddRef(ID3D12DescriptorHeap *iface)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);
    ULONG refcount = InterlockedIncrement(&heap->refcount);

    TRACE("%p increasing refcount to %u.\n", heap, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_descriptor_heap_Release(ID3D12DescriptorHeap *iface)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);
    ULONG refcount = InterlockedDecrement(&heap->refcount);

    TRACE("%p decreasing refcount to %u.\n", heap, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = heap->device;

        d3d12_descriptor_heap_cleanup(heap);
        vkd3d_private_store_destroy(&heap->private_store);
        vkd3d_free_aligned(heap);

        d3d12_device_release(device);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_GetPrivateData(ID3D12DescriptorHeap *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&heap->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_SetPrivateData(ID3D12DescriptorHeap *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&heap->private_store, guid, data_size, data,
            NULL, NULL);
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_SetPrivateDataInterface(ID3D12DescriptorHeap *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&heap->private_store, guid, data,
            NULL, NULL);
}

static HRESULT STDMETHODCALLTYPE d3d12_descriptor_heap_GetDevice(ID3D12DescriptorHeap *iface, REFIID iid, void **device)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, iid %s, device %p.\n", iface, debugstr_guid(iid), device);

    return d3d12_device_query_interface(heap->device, iid, device);
}

static D3D12_DESCRIPTOR_HEAP_DESC * STDMETHODCALLTYPE d3d12_descriptor_heap_GetDesc(ID3D12DescriptorHeap *iface,
        D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    *desc = heap->desc;
    return desc;
}

static D3D12_CPU_DESCRIPTOR_HANDLE * STDMETHODCALLTYPE d3d12_descriptor_heap_GetCPUDescriptorHandleForHeapStart(
        ID3D12DescriptorHeap *iface, D3D12_CPU_DESCRIPTOR_HANDLE *descriptor)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, descriptor %p.\n", iface, descriptor);

    descriptor->ptr = (SIZE_T)heap->descriptors;

    return descriptor;
}

static D3D12_GPU_DESCRIPTOR_HANDLE * STDMETHODCALLTYPE d3d12_descriptor_heap_GetGPUDescriptorHandleForHeapStart(
        ID3D12DescriptorHeap *iface, D3D12_GPU_DESCRIPTOR_HANDLE *descriptor)
{
    struct d3d12_descriptor_heap *heap = impl_from_ID3D12DescriptorHeap(iface);

    TRACE("iface %p, descriptor %p.\n", iface, descriptor);

    descriptor->ptr = heap->gpu_va;

    return descriptor;
}

CONST_VTBL struct ID3D12DescriptorHeapVtbl d3d12_descriptor_heap_vtbl =
{
    /* IUnknown methods */
    d3d12_descriptor_heap_QueryInterface,
    d3d12_descriptor_heap_AddRef,
    d3d12_descriptor_heap_Release,
    /* ID3D12Object methods */
    d3d12_descriptor_heap_GetPrivateData,
    d3d12_descriptor_heap_SetPrivateData,
    d3d12_descriptor_heap_SetPrivateDataInterface,
    (void *)d3d12_object_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_descriptor_heap_GetDevice,
    /* ID3D12DescriptorHeap methods */
    d3d12_descriptor_heap_GetDesc,
    d3d12_descriptor_heap_GetCPUDescriptorHandleForHeapStart,
    d3d12_descriptor_heap_GetGPUDescriptorHandleForHeapStart,
};

static HRESULT d3d12_descriptor_heap_create_descriptor_pool(struct d3d12_descriptor_heap *descriptor_heap,
        VkDescriptorPool *vk_descriptor_pool)
{
    const struct vkd3d_vk_device_procs *vk_procs = &descriptor_heap->device->vk_procs;
    VkDescriptorPoolSize vk_pool_sizes[VKD3D_MAX_BINDLESS_DESCRIPTOR_SETS];
    const struct d3d12_device *device = descriptor_heap->device;
    unsigned int i, pool_count = 0, ssbo_count = 0;
    VkDescriptorPoolCreateInfo vk_pool_info;
    VkDescriptorPoolSize *ssbo_pool = NULL;
    VkResult vr;

    for (i = 0; i < device->bindless_state.set_count; i++)
    {
        const struct vkd3d_bindless_set_info *set_info = &device->bindless_state.set_info[i];

        if (set_info->heap_type == descriptor_heap->desc.Type)
        {
            VkDescriptorPoolSize *vk_pool_size = &vk_pool_sizes[pool_count++];
            vk_pool_size->type = set_info->vk_descriptor_type;
            vk_pool_size->descriptorCount = descriptor_heap->desc.NumDescriptors;

            if (vkd3d_descriptor_debug_active_qa_checks() &&
                    descriptor_heap->desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
            {
                vk_pool_size->descriptorCount += VKD3D_DESCRIPTOR_DEBUG_NUM_PAD_DESCRIPTORS;
            }

            if (set_info->vk_descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                ssbo_pool = vk_pool_size;
        }

        ssbo_count += vkd3d_popcount(set_info->flags & VKD3D_BINDLESS_SET_EXTRA_MASK);
    }

    if (ssbo_count && !ssbo_pool)
    {
        ssbo_pool = &vk_pool_sizes[pool_count++];
        ssbo_pool->type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ssbo_pool->descriptorCount = 0;  /* see below */
    }

    if (ssbo_pool)
        ssbo_pool->descriptorCount += ssbo_count;

    if (!pool_count)
        return S_OK;

    /* If using mutable type, we will allocate the most conservative size.
     * This is fine since we're attempting to allocate a completely generic descriptor set. */

    vk_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    vk_pool_info.pNext = NULL;

    vk_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT;
    if (!(descriptor_heap->desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
            (descriptor_heap->device->bindless_state.flags & VKD3D_BINDLESS_MUTABLE_TYPE))
        vk_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_HOST_ONLY_BIT_VALVE;

    vk_pool_info.maxSets = pool_count;
    vk_pool_info.poolSizeCount = pool_count;
    vk_pool_info.pPoolSizes = vk_pool_sizes;

    if ((vr = VK_CALL(vkCreateDescriptorPool(device->vk_device,
            &vk_pool_info, NULL, vk_descriptor_pool))) < 0)
    {
        ERR("Failed to create descriptor pool, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    return S_OK;
}

static void d3d12_descriptor_heap_zero_initialize(struct d3d12_descriptor_heap *descriptor_heap,
        VkDescriptorType vk_descriptor_type, VkDescriptorSet vk_descriptor_set,
        uint32_t binding_index, uint32_t descriptor_count)
{
    const struct vkd3d_vk_device_procs *vk_procs = &descriptor_heap->device->vk_procs;
    const struct d3d12_device *device = descriptor_heap->device;
    VkDescriptorBufferInfo *buffer_infos = NULL;
    VkDescriptorImageInfo *image_infos = NULL;
    VkBufferView *buffer_view_infos = NULL;
    VkWriteDescriptorSet write;
    uint32_t i;

    /* Clear out descriptor heap with the largest possible descriptor type we know of when using mutable descriptor type.
     * Purely for defensive purposes. */
    if (vk_descriptor_type == VK_DESCRIPTOR_TYPE_MUTABLE_VALVE)
        vk_descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;

    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = NULL;
    write.descriptorType = vk_descriptor_type;
    write.dstSet = vk_descriptor_set;
    write.dstBinding = binding_index;
    write.dstArrayElement = 0;
    write.descriptorCount = descriptor_count;
    write.pTexelBufferView = NULL;
    write.pImageInfo = NULL;
    write.pBufferInfo = NULL;

    switch (vk_descriptor_type)
    {
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        image_infos = vkd3d_calloc(descriptor_count, sizeof(*image_infos));
        write.pImageInfo = image_infos;
        break;

    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        buffer_infos = vkd3d_calloc(descriptor_count, sizeof(*buffer_infos));
        write.pBufferInfo = buffer_infos;
        for (i = 0; i < descriptor_count; i++)
            buffer_infos[i].range = VK_WHOLE_SIZE;
        break;

    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        buffer_view_infos = vkd3d_calloc(descriptor_count, sizeof(*buffer_view_infos));
        write.pTexelBufferView = buffer_view_infos;
        break;

    default:
        break;
    }

    VK_CALL(vkUpdateDescriptorSets(device->vk_device, 1, &write, 0, NULL));
    vkd3d_free(image_infos);
    vkd3d_free(buffer_view_infos);
    vkd3d_free(buffer_infos);
}

static HRESULT d3d12_descriptor_heap_create_descriptor_set(struct d3d12_descriptor_heap *descriptor_heap,
        const struct vkd3d_bindless_set_info *binding, VkDescriptorSet *vk_descriptor_set)
{
    const struct vkd3d_vk_device_procs *vk_procs = &descriptor_heap->device->vk_procs;
    VkDescriptorSetVariableDescriptorCountAllocateInfoEXT vk_variable_count_info;
    uint32_t descriptor_count = descriptor_heap->desc.NumDescriptors;
    const struct d3d12_device *device = descriptor_heap->device;
    VkDescriptorSetAllocateInfo vk_set_info;
    VkResult vr;

    if (vkd3d_descriptor_debug_active_qa_checks() && descriptor_heap->desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        descriptor_count += VKD3D_DESCRIPTOR_DEBUG_NUM_PAD_DESCRIPTORS;

    vk_variable_count_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT;
    vk_variable_count_info.pNext = NULL;
    vk_variable_count_info.descriptorSetCount = 1;
    vk_variable_count_info.pDescriptorCounts = &descriptor_count;

    vk_set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    vk_set_info.pNext = &vk_variable_count_info;
    vk_set_info.descriptorPool = descriptor_heap->vk_descriptor_pool;
    vk_set_info.descriptorSetCount = 1;
    vk_set_info.pSetLayouts = &binding->vk_host_set_layout;

    if (descriptor_heap->desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        vk_set_info.pSetLayouts = &binding->vk_set_layout;

    if ((vr = VK_CALL(vkAllocateDescriptorSets(device->vk_device, &vk_set_info, vk_descriptor_set))) < 0)
    {
        ERR("Failed to allocate descriptor set, vr %d.\n", vr);
        return hresult_from_vk_result(vr);
    }

    if (binding->vk_descriptor_type != VK_DESCRIPTOR_TYPE_SAMPLER)
    {
        d3d12_descriptor_heap_zero_initialize(descriptor_heap,
                binding->vk_descriptor_type, *vk_descriptor_set,
                binding->binding_index, descriptor_count);
    }

    return S_OK;
}

static void d3d12_descriptor_heap_get_buffer_range(struct d3d12_descriptor_heap *descriptor_heap,
        VkDeviceSize *offset, VkDeviceSize size, struct vkd3d_host_visible_buffer_range *range)
{
    if (size)
    {
        range->descriptor.buffer = descriptor_heap->vk_buffer;
        range->descriptor.offset = *offset;
        range->descriptor.range = size;
        range->host_ptr = void_ptr_offset(descriptor_heap->host_memory, *offset);

        *offset += size;
    }
    else
    {
        range->descriptor.buffer = VK_NULL_HANDLE;
        range->descriptor.offset = 0;
        range->descriptor.range = VK_WHOLE_SIZE;
        range->host_ptr = NULL;
    }
}

static HRESULT d3d12_descriptor_heap_init_data_buffer(struct d3d12_descriptor_heap *descriptor_heap,
        struct d3d12_device *device, const D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
    const struct vkd3d_vk_device_procs *vk_procs = &descriptor_heap->device->vk_procs;
    VkDeviceSize alignment = max(device->device_info.properties2.properties.limits.minStorageBufferOffsetAlignment,
            device->device_info.properties2.properties.limits.nonCoherentAtomSize);
    VkDeviceSize raw_va_buffer_size = 0, offset_buffer_size = 0;
    VKD3D_UNUSED VkDeviceSize descriptor_heap_info_size = 0;
    VkMemoryPropertyFlags property_flags;
    VkDeviceSize buffer_size, offset;
    D3D12_HEAP_PROPERTIES heap_info;
    D3D12_RESOURCE_DESC buffer_desc;
    D3D12_HEAP_FLAGS heap_flags;
    VkResult vr;
    HRESULT hr;

    if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        if (device->bindless_state.flags & VKD3D_RAW_VA_AUX_BUFFER)
        {
            raw_va_buffer_size = align(desc->NumDescriptors * sizeof(VkDeviceAddress), alignment);
            if (vkd3d_descriptor_debug_active_qa_checks())
                raw_va_buffer_size += align(VKD3D_DESCRIPTOR_DEBUG_NUM_PAD_DESCRIPTORS * sizeof(VkDeviceAddress), alignment);
        }

        if (device->bindless_state.flags & (VKD3D_SSBO_OFFSET_BUFFER | VKD3D_TYPED_OFFSET_BUFFER))
            offset_buffer_size = align(desc->NumDescriptors * sizeof(struct vkd3d_bound_buffer_range), alignment);

        if (vkd3d_descriptor_debug_active_qa_checks())
            descriptor_heap_info_size = align(vkd3d_descriptor_debug_heap_info_size(desc->NumDescriptors), alignment);
    }

    buffer_size = raw_va_buffer_size + offset_buffer_size;
    buffer_size += descriptor_heap_info_size;

    if (!buffer_size)
        return S_OK;

    if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
    {
        memset(&buffer_desc, 0, sizeof(buffer_desc));
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = buffer_size;
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        /* host-visible device memory */
        memset(&heap_info, 0, sizeof(heap_info));
        heap_info.Type = D3D12_HEAP_TYPE_UPLOAD;

        heap_flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

        if (FAILED(hr = vkd3d_create_buffer(device, &heap_info, heap_flags, &buffer_desc, &descriptor_heap->vk_buffer)))
            return hr;

        property_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (vkd3d_config_flags & VKD3D_CONFIG_FLAG_FORCE_HOST_CACHED)
            property_flags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        else if (!(vkd3d_config_flags & VKD3D_CONFIG_FLAG_NO_UPLOAD_HVV))
            property_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        if (FAILED(hr = vkd3d_allocate_buffer_memory(device, descriptor_heap->vk_buffer,
                property_flags, &descriptor_heap->device_allocation)))
            return hr;

        if ((vr = VK_CALL(vkMapMemory(device->vk_device, descriptor_heap->device_allocation.vk_memory,
                0, VK_WHOLE_SIZE, 0, &descriptor_heap->host_memory))))
        {
            ERR("Failed to map buffer, vr %d.\n", vr);
            return hresult_from_vk_result(vr);
        }
    }
    else
    {
        memset(&descriptor_heap->device_allocation, 0, sizeof(descriptor_heap->device_allocation));
        descriptor_heap->vk_buffer = VK_NULL_HANDLE;
        descriptor_heap->host_memory = vkd3d_calloc(1, buffer_size);
    }

    offset = 0;

    d3d12_descriptor_heap_get_buffer_range(descriptor_heap, &offset, raw_va_buffer_size, &descriptor_heap->raw_va_aux_buffer);
    d3d12_descriptor_heap_get_buffer_range(descriptor_heap, &offset, offset_buffer_size, &descriptor_heap->buffer_ranges);
#ifdef VKD3D_ENABLE_DESCRIPTOR_QA
    d3d12_descriptor_heap_get_buffer_range(descriptor_heap, &offset,
            descriptor_heap_info_size,
            &descriptor_heap->descriptor_heap_info);
#endif
    return S_OK;
}

static void d3d12_descriptor_heap_update_extra_bindings(struct d3d12_descriptor_heap *descriptor_heap,
        struct d3d12_device *device)
{
    VkDescriptorBufferInfo vk_buffer_info[VKD3D_BINDLESS_SET_MAX_EXTRA_BINDINGS];
    VkWriteDescriptorSet vk_writes[VKD3D_BINDLESS_SET_MAX_EXTRA_BINDINGS];
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    unsigned int i, binding_index, set_index = 0, write_count = 0;
    uint32_t flags;

    for (i = 0; i < device->bindless_state.set_count; i++)
    {
        const struct vkd3d_bindless_set_info *set_info = &device->bindless_state.set_info[i];

        if (set_info->heap_type != descriptor_heap->desc.Type)
            continue;

        flags = set_info->flags & VKD3D_BINDLESS_SET_EXTRA_MASK;
        binding_index = 0;

        while (flags)
        {
            enum vkd3d_bindless_set_flag flag = (enum vkd3d_bindless_set_flag)(flags & -flags);
            VkDescriptorBufferInfo *vk_buffer = &vk_buffer_info[write_count];
            VkWriteDescriptorSet *vk_write = &vk_writes[write_count];

            vk_write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            vk_write->pNext = NULL;
            vk_write->dstSet = descriptor_heap->vk_descriptor_sets[set_index];
            vk_write->dstBinding = binding_index++;
            vk_write->dstArrayElement = 0;
            vk_write->descriptorCount = 1;
            vk_write->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            vk_write->pImageInfo = NULL;
            vk_write->pBufferInfo = vk_buffer;
            vk_write->pTexelBufferView = NULL;

            switch (flag)
            {
                case VKD3D_BINDLESS_SET_EXTRA_RAW_VA_AUX_BUFFER:
                    *vk_buffer = descriptor_heap->raw_va_aux_buffer.descriptor;
                    break;

                case VKD3D_BINDLESS_SET_EXTRA_OFFSET_BUFFER:
                    *vk_buffer = descriptor_heap->buffer_ranges.descriptor;
                    break;

#ifdef VKD3D_ENABLE_DESCRIPTOR_QA
                case VKD3D_BINDLESS_SET_EXTRA_GLOBAL_HEAP_INFO_BUFFER:
                    *vk_buffer = *vkd3d_descriptor_debug_get_global_info_descriptor(device->descriptor_qa_global_info);
                    break;

                case VKD3D_BINDLESS_SET_EXTRA_DESCRIPTOR_HEAP_INFO_BUFFER:
                    *vk_buffer = descriptor_heap->descriptor_heap_info.descriptor;
                    break;
#endif

                default:
                    ERR("Unsupported extra flags %#x.\n", flag);
                    continue;
            }

            write_count += 1;
            flags -= flag;
        }

        set_index += 1;
    }

    if (write_count)
        VK_CALL(vkUpdateDescriptorSets(device->vk_device, write_count, vk_writes, 0, NULL));
}

static void d3d12_descriptor_heap_add_null_descriptor_template(
        struct d3d12_descriptor_heap *descriptor_heap,
        const struct vkd3d_bindless_set_info *set_info,
        unsigned int set_info_index)
{
    struct VkWriteDescriptorSet *write;
    unsigned int index;

    index = descriptor_heap->null_descriptor_template.num_writes;

    write = &descriptor_heap->null_descriptor_template.writes[index];
    write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write->pNext = NULL;
    write->descriptorCount = 1;
    write->dstSet = descriptor_heap->vk_descriptor_sets[set_info->set_index];
    write->dstBinding = set_info->binding_index;

    /* Replaced when instantiating template. */
    write->dstArrayElement = 0;

    /* For mutable, will be replaced when instantiating template. */
    write->descriptorType = set_info->vk_descriptor_type;

    write->pBufferInfo = &descriptor_heap->null_descriptor_template.buffer;
    write->pImageInfo = &descriptor_heap->null_descriptor_template.image;
    write->pTexelBufferView = &descriptor_heap->null_descriptor_template.buffer_view;

    if (index == 0)
    {
        descriptor_heap->null_descriptor_template.buffer.offset = 0;
        descriptor_heap->null_descriptor_template.buffer.range = VK_WHOLE_SIZE;
        descriptor_heap->null_descriptor_template.buffer.buffer = VK_NULL_HANDLE;
        descriptor_heap->null_descriptor_template.image.sampler = VK_NULL_HANDLE;
        descriptor_heap->null_descriptor_template.image.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        descriptor_heap->null_descriptor_template.image.imageView = VK_NULL_HANDLE;
        descriptor_heap->null_descriptor_template.buffer_view = VK_NULL_HANDLE;
        descriptor_heap->null_descriptor_template.has_mutable_descriptors =
                descriptor_heap->device->vk_info.VALVE_mutable_descriptor_type;
    }

    descriptor_heap->null_descriptor_template.num_writes++;
    descriptor_heap->null_descriptor_template.set_info_mask |= 1u << set_info_index;
}

static HRESULT d3d12_descriptor_heap_init(struct d3d12_descriptor_heap *descriptor_heap,
        struct d3d12_device *device, const D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
    unsigned int i;
    HRESULT hr;

    memset(descriptor_heap, 0, sizeof(*descriptor_heap));
    descriptor_heap->ID3D12DescriptorHeap_iface.lpVtbl = &d3d12_descriptor_heap_vtbl;
    descriptor_heap->refcount = 1;
    descriptor_heap->device = device;
    descriptor_heap->desc = *desc;

    if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        descriptor_heap->gpu_va = d3d12_device_get_descriptor_heap_gpu_va(device);

    if (FAILED(hr = d3d12_descriptor_heap_create_descriptor_pool(descriptor_heap,
            &descriptor_heap->vk_descriptor_pool)))
        goto fail;

    if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
            desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
    {
        for (i = 0; i < device->bindless_state.set_count; i++)
        {
            const struct vkd3d_bindless_set_info *set_info = &device->bindless_state.set_info[i];

            if (set_info->heap_type == desc->Type)
            {
                if (FAILED(hr = d3d12_descriptor_heap_create_descriptor_set(descriptor_heap,
                        set_info, &descriptor_heap->vk_descriptor_sets[set_info->set_index])))
                    goto fail;

                if (descriptor_heap->desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                    d3d12_descriptor_heap_add_null_descriptor_template(descriptor_heap, set_info, i);
            }
        }
    }

    if (FAILED(hr = d3d12_descriptor_heap_init_data_buffer(descriptor_heap, device, desc)))
        goto fail;

    if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        d3d12_descriptor_heap_update_extra_bindings(descriptor_heap, device);

    if (FAILED(hr = vkd3d_private_store_init(&descriptor_heap->private_store)))
        goto fail;

    d3d12_device_add_ref(descriptor_heap->device);
    return S_OK;

fail:
    d3d12_descriptor_heap_cleanup(descriptor_heap);
    return hr;
}

static void d3d12_descriptor_heap_init_descriptors(struct d3d12_descriptor_heap *descriptor_heap,
        size_t descriptor_size)
{
    struct d3d12_desc *desc;
    unsigned int i;

    memset(descriptor_heap->descriptors, 0, descriptor_size * descriptor_heap->desc.NumDescriptors);

    switch (descriptor_heap->desc.Type)
    {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
            desc = (struct d3d12_desc *)descriptor_heap->descriptors;

            for (i = 0; i < descriptor_heap->desc.NumDescriptors; i++)
            {
                desc[i].heap = descriptor_heap;
                desc[i].heap_offset = i;
                /* If we begin copying from the descriptors right away, we should copy the null descriptors
                 * which are already initialized. */
                desc[i].metadata.set_info_mask = descriptor_heap->null_descriptor_template.set_info_mask;
            }
            break;

        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
            break;

        default:
            WARN("Unhandled descriptor heap type: %d.\n", descriptor_heap->desc.Type);
    }
}

HRESULT d3d12_descriptor_heap_create(struct d3d12_device *device,
        const D3D12_DESCRIPTOR_HEAP_DESC *desc, struct d3d12_descriptor_heap **descriptor_heap)
{
    size_t max_descriptor_count, descriptor_size;
    struct d3d12_descriptor_heap *object;
    HRESULT hr;

    if (!(descriptor_size = d3d12_device_get_descriptor_handle_increment_size(device, desc->Type)))
    {
        WARN("No descriptor size for descriptor type %#x.\n", desc->Type);
        return E_INVALIDARG;
    }

    if ((desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
            && (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV))
    {
        WARN("RTV/DSV descriptor heaps cannot be shader visible.\n");
        return E_INVALIDARG;
    }

    max_descriptor_count = (UINT32_MAX - sizeof(*object)) / descriptor_size;
    if (desc->NumDescriptors > max_descriptor_count)
    {
        WARN("Invalid descriptor count %u (max %zu).\n", desc->NumDescriptors, max_descriptor_count);
        return E_OUTOFMEMORY;
    }

    if (!(object = vkd3d_malloc_aligned(offsetof(struct d3d12_descriptor_heap,
            descriptors[descriptor_size * desc->NumDescriptors]), D3D12_DESC_ALIGNMENT)))
        return E_OUTOFMEMORY;

    if (FAILED(hr = d3d12_descriptor_heap_init(object, device, desc)))
    {
        vkd3d_free_aligned(object);
        return hr;
    }

    d3d12_descriptor_heap_init_descriptors(object, descriptor_size);

    TRACE("Created descriptor heap %p.\n", object);

#ifdef VKD3D_ENABLE_DESCRIPTOR_QA
    object->cookie = vkd3d_allocate_cookie();
    vkd3d_descriptor_debug_register_heap(object->descriptor_heap_info.host_ptr, object->cookie, desc);
#endif

    *descriptor_heap = object;

    return S_OK;
}

void d3d12_descriptor_heap_cleanup(struct d3d12_descriptor_heap *descriptor_heap)
{
    const struct vkd3d_vk_device_procs *vk_procs = &descriptor_heap->device->vk_procs;
    struct d3d12_device *device = descriptor_heap->device;

    if (!descriptor_heap->device_allocation.vk_memory)
        vkd3d_free(descriptor_heap->host_memory);

    if (descriptor_heap->gpu_va != 0)
        d3d12_device_return_descriptor_heap_gpu_va(device, descriptor_heap->gpu_va);

    VK_CALL(vkDestroyBuffer(device->vk_device, descriptor_heap->vk_buffer, NULL));
    vkd3d_free_device_memory(device, &descriptor_heap->device_allocation);

    VK_CALL(vkDestroyDescriptorPool(device->vk_device, descriptor_heap->vk_descriptor_pool, NULL));

    vkd3d_descriptor_debug_unregister_heap(descriptor_heap->cookie);
}

static void d3d12_query_heap_set_name(struct d3d12_query_heap *heap, const char *name)
{
    if (heap->vk_query_pool)
    {
        vkd3d_set_vk_object_name(heap->device, (uint64_t)heap->vk_query_pool,
                VK_OBJECT_TYPE_QUERY_POOL, name);
    }
    else /*if (heap->vk_buffer)*/
    {
        vkd3d_set_vk_object_name(heap->device, (uint64_t)heap->vk_buffer,
                VK_OBJECT_TYPE_BUFFER, name);
    }
}

/* ID3D12QueryHeap */
static HRESULT STDMETHODCALLTYPE d3d12_query_heap_QueryInterface(ID3D12QueryHeap *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID3D12QueryHeap)
            || IsEqualGUID(iid, &IID_ID3D12Pageable)
            || IsEqualGUID(iid, &IID_ID3D12DeviceChild)
            || IsEqualGUID(iid, &IID_ID3D12Object)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        ID3D12QueryHeap_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d12_query_heap_AddRef(ID3D12QueryHeap *iface)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);
    ULONG refcount = InterlockedIncrement(&heap->refcount);

    TRACE("%p increasing refcount to %u.\n", heap, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d12_query_heap_Release(ID3D12QueryHeap *iface)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);
    ULONG refcount = InterlockedDecrement(&heap->refcount);

    TRACE("%p decreasing refcount to %u.\n", heap, refcount);

    if (!refcount)
    {
        struct d3d12_device *device = heap->device;
        const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;

        vkd3d_private_store_destroy(&heap->private_store);

        VK_CALL(vkDestroyQueryPool(device->vk_device, heap->vk_query_pool, NULL));
        VK_CALL(vkDestroyBuffer(device->vk_device, heap->vk_buffer, NULL));
        vkd3d_free_device_memory(device, &heap->device_allocation);

        vkd3d_free(heap);

        d3d12_device_release(device);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_GetPrivateData(ID3D12QueryHeap *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_get_private_data(&heap->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_SetPrivateData(ID3D12QueryHeap *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return vkd3d_set_private_data(&heap->private_store, guid, data_size, data,
            (vkd3d_set_name_callback) d3d12_query_heap_set_name, heap);
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_SetPrivateDataInterface(ID3D12QueryHeap *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return vkd3d_set_private_data_interface(&heap->private_store, guid, data,
            (vkd3d_set_name_callback) d3d12_query_heap_set_name, heap);
}

static HRESULT STDMETHODCALLTYPE d3d12_query_heap_GetDevice(ID3D12QueryHeap *iface, REFIID iid, void **device)
{
    struct d3d12_query_heap *heap = impl_from_ID3D12QueryHeap(iface);

    TRACE("iface %p, iid %s, device %p.\n", iface, debugstr_guid(iid), device);

    return d3d12_device_query_interface(heap->device, iid, device);
}

CONST_VTBL struct ID3D12QueryHeapVtbl d3d12_query_heap_vtbl =
{
    /* IUnknown methods */
    d3d12_query_heap_QueryInterface,
    d3d12_query_heap_AddRef,
    d3d12_query_heap_Release,
    /* ID3D12Object methods */
    d3d12_query_heap_GetPrivateData,
    d3d12_query_heap_SetPrivateData,
    d3d12_query_heap_SetPrivateDataInterface,
    (void *)d3d12_object_SetName,
    /* ID3D12DeviceChild methods */
    d3d12_query_heap_GetDevice,
};

HRESULT d3d12_query_heap_create(struct d3d12_device *device, const D3D12_QUERY_HEAP_DESC *desc,
        struct d3d12_query_heap **heap)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    D3D12_HEAP_PROPERTIES heap_properties;
    struct d3d12_query_heap *object;
    VkQueryPoolCreateInfo pool_info;
    D3D12_RESOURCE_DESC buffer_desc;
    VkResult vr;
    HRESULT hr;

    if (!(object = vkd3d_malloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    memset(object, 0, sizeof(*object));
    object->ID3D12QueryHeap_iface.lpVtbl = &d3d12_query_heap_vtbl;
    object->refcount = 1;
    object->device = device;
    object->desc = *desc;

    if (!d3d12_query_heap_type_is_inline(desc->Type))
    {
        pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        pool_info.pNext = NULL;
        pool_info.flags = 0;
        pool_info.queryCount = desc->Count;

        switch (desc->Type)
        {
            case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
                pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
                pool_info.pipelineStatistics = 0;
                break;

            case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
                pool_info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
                pool_info.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT
                        | VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
                break;

            default:
                WARN("Invalid query heap type %u.\n", desc->Type);
                vkd3d_free(object);
                return E_INVALIDARG;
        }

        if ((vr = VK_CALL(vkCreateQueryPool(device->vk_device, &pool_info, NULL, &object->vk_query_pool))) < 0)
        {
            WARN("Failed to create Vulkan query pool, vr %d.\n", vr);
            vkd3d_free(object);
            return hresult_from_vk_result(vr);
        }
    }
    else
    {
        memset(&heap_properties, 0, sizeof(heap_properties));
        heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Alignment = 0;
        buffer_desc.Width = d3d12_query_heap_type_get_data_size(desc->Type) * desc->Count;
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.SampleDesc.Quality = 0;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (FAILED(hr = vkd3d_create_buffer(device, &heap_properties,
                D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, &buffer_desc, &object->vk_buffer)))
        {
            vkd3d_free(object);
            return hr;
        }

        if (FAILED(hr = vkd3d_allocate_buffer_memory(device, object->vk_buffer,
                VK_MEMORY_HEAP_DEVICE_LOCAL_BIT, &object->device_allocation)))
        {
            VK_CALL(vkDestroyBuffer(device->vk_device, object->vk_buffer, NULL));
            vkd3d_free(object);
            return hr;
        }

        /* Explicit initialization is not required for these since
         * we can expect the buffer to be zero-initialized. */
        object->initialized = 1;
    }

    if (FAILED(hr = vkd3d_private_store_init(&object->private_store)))
    {
        vkd3d_free(object);
        return hr;
    }

    d3d12_device_add_ref(device);

    TRACE("Created query heap %p.\n", object);

    *heap = object;
    return S_OK;
}

struct vkd3d_memory_topology
{
    VkDeviceSize largest_device_local_heap_size;
    VkDeviceSize largest_host_only_heap_size;
    uint32_t largest_device_local_heap_index;
    uint32_t largest_host_only_heap_index;
    uint32_t device_local_heap_count;
    uint32_t host_only_heap_count;
    bool exists_device_only_type;
    bool exists_host_only_type;
};

static void vkd3d_memory_info_get_topology(struct vkd3d_memory_topology *topology,
        struct d3d12_device *device)
{
    VkMemoryPropertyFlags flags;
    VkDeviceSize heap_size;
    uint32_t heap_index;
    unsigned int i;

    memset(topology, 0, sizeof(*topology));

    for (i = 0; i < device->memory_properties.memoryHeapCount; i++)
    {
        heap_size = device->memory_properties.memoryHeaps[i].size;
        if (device->memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            if (heap_size > topology->largest_device_local_heap_size)
            {
                topology->largest_device_local_heap_index = i;
                topology->largest_device_local_heap_size = heap_size;
            }
            topology->device_local_heap_count++;
        }
        else
        {
            if (heap_size > topology->largest_host_only_heap_size)
            {
                topology->largest_host_only_heap_index = i;
                topology->largest_host_only_heap_size = heap_size;
            }
            topology->host_only_heap_count++;
        }
    }

    for (i = 0; i < device->memory_properties.memoryTypeCount; i++)
    {
        flags = device->memory_properties.memoryTypes[i].propertyFlags;
        heap_index = device->memory_properties.memoryTypes[i].heapIndex;

        if (heap_index == topology->largest_device_local_heap_index &&
                (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
        {
            topology->exists_device_only_type = true;
        }
        else if (heap_index == topology->largest_host_only_heap_index &&
                (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0 &&
                (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            topology->exists_host_only_type = true;
        }
    }
}

static uint32_t vkd3d_memory_info_find_global_mask(const struct vkd3d_memory_topology *topology, struct d3d12_device *device)
{
    /* Never allow memory types from any PCI-pinned heap.
     * If we allow it, it might end up being used as a fallback memory type, which will cause severe instabilities.
     * These types should only be used in a controlled fashion. */
    VkMemoryPropertyFlags flags;
    uint32_t heap_index;
    uint32_t i, mask;

    if (!(vkd3d_config_flags & VKD3D_CONFIG_FLAG_NO_UPLOAD_HVV))
        return UINT32_MAX;

    /* If we only have one device local heap, or no host-only heaps, there is nothing to do. */
    if (topology->device_local_heap_count <= 1 || topology->host_only_heap_count == 0)
        return UINT32_MAX;

    /* Verify that there exists a DEVICE_LOCAL type that is not HOST_VISIBLE on this device
     * which maps to the largest device local heap. That way, it is safe to mask out all memory types which are
     * DEVICE_LOCAL | HOST_VISIBLE.
     * Similarly, there must exist a host-only type. */
    if (!topology->exists_device_only_type || !topology->exists_host_only_type)
        return UINT32_MAX;

    /* Mask out any memory types which are deemed problematic. */
    for (i = 0, mask = 0; i < device->memory_properties.memoryTypeCount; i++)
    {
        const VkMemoryPropertyFlags pinned_mask = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        flags = device->memory_properties.memoryTypes[i].propertyFlags;
        heap_index = device->memory_properties.memoryTypes[i].heapIndex;

        if (heap_index != topology->largest_device_local_heap_index &&
            heap_index != topology->largest_host_only_heap_index &&
            (flags & pinned_mask) == pinned_mask)
        {
            mask |= 1u << i;
            WARN("Blocking memory type %u for use (PCI-pinned memory).\n", i);
        }
    }

    return ~mask;
}

static void vkd3d_memory_info_init_budgets(struct vkd3d_memory_info *info,
        const struct vkd3d_memory_topology *topology,
        struct d3d12_device *device)
{
    bool heap_index_needs_budget;
    VkMemoryPropertyFlags flags;
    uint32_t heap_index;
    uint32_t i;

    info->budget_sensitive_mask = 0;

    /* Nothing to do if we don't have separate heaps. */
    if (topology->device_local_heap_count == 0 || topology->host_only_heap_count == 0)
        return;
    if (!topology->exists_device_only_type || !topology->exists_host_only_type)
        return;

    for (i = 0; i < device->memory_properties.memoryTypeCount; i++)
    {
        const VkMemoryPropertyFlags pinned_mask = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        flags = device->memory_properties.memoryTypes[i].propertyFlags;
        heap_index = device->memory_properties.memoryTypes[i].heapIndex;

        /* Work around a driver workaround on NV drivers which targets certain
         * older DXVK versions (use of DXVK DXGI is likely what impacts us here),
         * since we don't see this behavior in native builds.
         * Even with resizable BAR, we might observe two different heaps,
         * with very slightly different heap sizes.
         * It's straight forward to be universally robust against these kinds of scenarios,
         * so just go for that.
         * If we're within 75% of the actual VRAM size, assume we've hit this scenario.
         * This should exclude small BAR from explicit budget, since that's just 256 MB. */
        heap_index_needs_budget =
                (device->memory_properties.memoryHeaps[heap_index].size >
                    3 * device->memory_properties.memoryHeaps[topology->largest_device_local_heap_index].size / 4) &&
                (device->memory_properties.memoryHeaps[heap_index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);

        if ((flags & pinned_mask) == pinned_mask && heap_index_needs_budget)
        {
            /* Limit this type. This limit is a pure heuristic and we might need further tuning here.
             * If there's a separate heap type for PCI-e BAR,
             * don't bother limiting it since the size is already going to be tiny.
             * The driver will limit us naturally. */
            info->budget_sensitive_mask |= 1u << i;
            info->type_budget[i] = device->memory_properties.memoryHeaps[heap_index].size / 16;
            info->type_current[i] = 0;
        }
    }

    INFO("Applying resizable BAR budget to memory types: 0x%x.\n", info->budget_sensitive_mask);
}

void vkd3d_memory_info_cleanup(struct vkd3d_memory_info *info,
        struct d3d12_device *device)
{
    pthread_mutex_destroy(&info->budget_lock);
}

HRESULT vkd3d_memory_info_init(struct vkd3d_memory_info *info,
        struct d3d12_device *device)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkMemoryRequirements memory_requirements;
    struct vkd3d_memory_topology topology;
    VkBufferCreateInfo buffer_info;
    uint32_t sampled_type_mask_cpu;
    VkImageCreateInfo image_info;
    uint32_t rt_ds_type_mask_cpu;
    uint32_t sampled_type_mask;
    uint32_t host_visible_mask;
    uint32_t buffer_type_mask;
    uint32_t rt_ds_type_mask;
    VkBuffer buffer;
    VkImage image;
    VkResult vr;
    uint32_t i;

    vkd3d_memory_info_get_topology(&topology, device);
    info->global_mask = vkd3d_memory_info_find_global_mask(&topology, device);
    vkd3d_memory_info_init_budgets(info, &topology, device);

    if (pthread_mutex_init(&info->budget_lock, NULL) != 0)
        return E_OUTOFMEMORY;

    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = 65536;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

    if (device->device_info.acceleration_structure_features.accelerationStructure)
    {
        /* Caps are not necessarily overridden yet.
         * Enabling RTAS should not change acceptable memory mask, but to be safe ... */
        buffer_info.usage |=
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    if ((vr = VK_CALL(vkCreateBuffer(device->vk_device, &buffer_info, NULL, &buffer))) < 0)
    {
        ERR("Failed to create dummy buffer");
        return hresult_from_vk_result(vr);
    }

    VK_CALL(vkGetBufferMemoryRequirements(device->vk_device, buffer, &memory_requirements));
    VK_CALL(vkDestroyBuffer(device->vk_device, buffer, NULL));
    buffer_type_mask = memory_requirements.memoryTypeBits;

    memset(&image_info, 0, sizeof(image_info));
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = 16;
    image_info.extent.height = 16;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if ((vr = VK_CALL(vkCreateImage(device->vk_device, &image_info, NULL, &image))) < 0)
    {
        ERR("Failed to create dummy sampled image");
        return hresult_from_vk_result(vr);
    }

    VK_CALL(vkGetImageMemoryRequirements(device->vk_device, image, &memory_requirements));
    VK_CALL(vkDestroyImage(device->vk_device, image, NULL));
    sampled_type_mask = memory_requirements.memoryTypeBits;

    /* CPU accessible images are always LINEAR.
     * If we ever get a way to write to OPTIMAL-ly tiled images, we can drop this and just
     * do sampled_type_mask_cpu & host_visible_set. */
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;
    /* Deliberately omit STORAGE_BIT here, since it's not supported at all on NV with HOST_VISIBLE.
     * Probably not 100% correct, but we can fix this if we get host visible OPTIMAL at some point. */
    sampled_type_mask_cpu = 0;
    if (vkd3d_is_linear_tiling_supported(device, &image_info))
    {
        if ((vr = VK_CALL(vkCreateImage(device->vk_device, &image_info, NULL, &image))) == VK_SUCCESS)
        {
            VK_CALL(vkGetImageMemoryRequirements(device->vk_device, image, &memory_requirements));
            VK_CALL(vkDestroyImage(device->vk_device, image, NULL));
            sampled_type_mask_cpu = memory_requirements.memoryTypeBits;
        }
    }
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT;

    if ((vr = VK_CALL(vkCreateImage(device->vk_device, &image_info, NULL, &image))) < 0)
    {
        ERR("Failed to create dummy color image");
        return hresult_from_vk_result(vr);
    }

    VK_CALL(vkGetImageMemoryRequirements(device->vk_device, image, &memory_requirements));
    VK_CALL(vkDestroyImage(device->vk_device, image, NULL));
    rt_ds_type_mask = memory_requirements.memoryTypeBits;

    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    rt_ds_type_mask_cpu = 0;
    if (vkd3d_is_linear_tiling_supported(device, &image_info))
    {
        if ((vr = VK_CALL(vkCreateImage(device->vk_device, &image_info, NULL, &image))) == VK_SUCCESS)
        {
            VK_CALL(vkGetImageMemoryRequirements(device->vk_device, image, &memory_requirements));
            VK_CALL(vkDestroyImage(device->vk_device, image, NULL));
            rt_ds_type_mask_cpu = memory_requirements.memoryTypeBits;
        }
    }
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    image_info.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;

    if ((vr = VK_CALL(vkCreateImage(device->vk_device, &image_info, NULL, &image))) < 0)
    {
        ERR("Failed to create dummy depth-stencil image");
        return hresult_from_vk_result(vr);
    }

    VK_CALL(vkGetImageMemoryRequirements(device->vk_device, image, &memory_requirements));
    VK_CALL(vkDestroyImage(device->vk_device, image, NULL));
    rt_ds_type_mask &= memory_requirements.memoryTypeBits;

    /* Unsure if we can have host visible depth-stencil.
     * On AMD, we can get linear RT, but not linear DS, so for now, just don't check for that.
     * We will fail in resource creation instead. */

    buffer_type_mask &= info->global_mask;
    sampled_type_mask &= info->global_mask;
    rt_ds_type_mask &= info->global_mask;
    sampled_type_mask_cpu &= info->global_mask;
    rt_ds_type_mask_cpu &= info->global_mask;

    info->non_cpu_accessible_domain.buffer_type_mask = buffer_type_mask;
    info->non_cpu_accessible_domain.sampled_type_mask = sampled_type_mask;
    info->non_cpu_accessible_domain.rt_ds_type_mask = rt_ds_type_mask;

    host_visible_mask = 0;
    for (i = 0; i < device->memory_properties.memoryTypeCount; i++)
        if (device->memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            host_visible_mask |= 1u << i;

    info->cpu_accessible_domain.buffer_type_mask = buffer_type_mask & host_visible_mask;
    info->cpu_accessible_domain.sampled_type_mask = sampled_type_mask_cpu & host_visible_mask;
    info->cpu_accessible_domain.rt_ds_type_mask = rt_ds_type_mask_cpu & host_visible_mask;

    /* If we cannot support linear render targets, this is fine.
     * If we don't fix this up here, we will fail to create a host visible TIER_2 heap.
     * Ignore any requirements for color attachments since we're never going to use it anyways. */
    if (info->cpu_accessible_domain.rt_ds_type_mask == 0 ||
            (vkd3d_config_flags & VKD3D_CONFIG_FLAG_IGNORE_RTV_HOST_VISIBLE))
        info->cpu_accessible_domain.rt_ds_type_mask = info->cpu_accessible_domain.sampled_type_mask;

    TRACE("Device supports buffers on memory types 0x%#x.\n", buffer_type_mask);
    TRACE("Device supports textures on memory types 0x%#x.\n", sampled_type_mask);
    TRACE("Device supports render targets on memory types 0x%#x.\n", rt_ds_type_mask);
    TRACE("Device supports CPU visible textures on memory types 0x%#x.\n",
          info->cpu_accessible_domain.sampled_type_mask);
    TRACE("Device supports CPU visible render targets on memory types 0x%#x.\n",
          info->cpu_accessible_domain.rt_ds_type_mask);
    return S_OK;
}
