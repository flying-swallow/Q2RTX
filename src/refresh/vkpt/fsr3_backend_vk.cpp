/*
 * Vulkan backend for the FidelityFX FSR 3 upscaler, built on the renderer's
 * existing device.  The SDK drives this through the FfxInterface callbacks:
 * it creates its internal textures at context creation, registers the
 * renderer's images once per dispatch, schedules barrier/clear/compute jobs
 * and finally asks execute() to record them into the frame command buffer.
 *
 * Conventions used here:
 *  - Every image stays in VK_IMAGE_LAYOUT_GENERAL.  The renderer keeps its
 *    own images there, and using one layout for the SDK's textures removes a
 *    class of layout-mismatch bugs at the cost of nothing measurable.
 *  - Shader bindings come from the reflection captured at shader-compile time
 *    (see fsr3/compile_fsr3_shader.py).  Register spaces are kept apart by
 *    binding shifts: b -> 0+, t -> 1000+, s -> 2000+, u -> 3000+.
 *  - Constant buffers are streamed through one host-visible ring buffer and
 *    bound as plain uniform buffers.
 */

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <new>
#include <vector>

#include "fsr3_backend_vk.h"
#include "fsr3_backend_vk_private.h"
#include "fsr3/fsr3_vkpt_blobs.h"

/* Defined below; the SDK links against this symbol as well. */
FfxErrorCode GetResourceSizeFromDescription(FfxDevice device, const FfxCreateResourceDescription* description,
                                             uint64_t* size, uint64_t* alignment);

namespace {

constexpr uint32_t kQueuedFrames = FFX_MAX_QUEUED_FRAMES;

struct Resource {
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    FfxApiResourceDescription desc{};
    FfxApiResourceState state = FFX_API_RESOURCE_STATE_COMMON;
    bool owned = false;      // created through create_resource()
    bool transient = false;  // registered for the current dispatch only
    bool layoutReady = false; // owned images: UNDEFINED -> GENERAL recorded
    Resource* alias = nullptr; // registered wrapper of an image we own
};

static bool& layout_flag(Resource* r) { return r->alias ? r->alias->layoutReady : r->layoutReady; }

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    std::vector<VkSampler> samplers;
    std::vector<uint32_t> samplerSlots;
};

struct Job { FfxGpuJobDescription job{}; };

struct Upload {
    int resourceIndex = -1;
    BufferResource_t staging{};
    VkDeviceSize size = 0;
};

struct ViewKey {
    VkImage image;
    VkFormat format;
    uint32_t mip;
    bool operator<(const ViewKey& o) const {
        if (image != o.image) return image < o.image;
        if (format != o.format) return format < o.format;
        return mip < o.mip;
    }
};

struct Backend {
    std::vector<Resource*> resources;
    std::vector<Pipeline*> pipelines;
    std::vector<Job> jobs;
    BufferResource_t constants{};
    char* constantsMapped = nullptr;
    uint32_t constantOffset = 0;
    VkDeviceSize uboAlignment = 256;
    VkDescriptorPool pools[kQueuedFrames] = {};
    uint32_t poolIndex = 0;
    std::map<ViewKey, VkImageView> views;
    std::vector<Upload> pendingUploads;
    std::vector<BufferResource_t> retiredStaging;
};

static void logf(const char* fmt, ...)
{
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    fsr3_vkpt_log(message);
}

static Backend* backend(FfxInterface* i) { return static_cast<Backend*>(i ? i->scratchBuffer : nullptr); }

static Resource* resource(Backend* b, FfxResourceInternal h)
{
    return b && h.internalIndex >= 0 && (size_t)h.internalIndex < b->resources.size() ? b->resources[h.internalIndex] : nullptr;
}

static FfxErrorCode unsupported() { return FFX_ERROR_INVALID_ARGUMENT; }

static VkPipelineStageFlags stages(FfxApiResourceState s)
{
    if (s & (FFX_API_RESOURCE_STATE_COPY_SRC | FFX_API_RESOURCE_STATE_COPY_DEST)) return VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (s & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT) return VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
}

static VkAccessFlags access(FfxApiResourceState s)
{
    VkAccessFlags a = 0;
    if (s & FFX_API_RESOURCE_STATE_COPY_SRC) a |= VK_ACCESS_TRANSFER_READ_BIT;
    if (s & FFX_API_RESOURCE_STATE_COPY_DEST) a |= VK_ACCESS_TRANSFER_WRITE_BIT;
    if (s & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) a |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    if (s & (FFX_API_RESOURCE_STATE_COMPUTE_READ | FFX_API_RESOURCE_STATE_PIXEL_READ)) a |= VK_ACCESS_SHADER_READ_BIT;
    if (s & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT) a |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    return a;
}

/* Record an execution/memory dependency for one resource.  Layouts never
 * change (everything is GENERAL), so the barrier is emitted even when the
 * states match: that is how UAV -> UAV hazards between passes are covered. */
static void transition(VkCommandBuffer cmd, Resource* r, FfxApiResourceState before, FfxApiResourceState after,
                       uint32_t mip = VK_REMAINING_MIP_LEVELS)
{
    if (!r) return;
    if (r->image) {
        if (!layout_flag(r)) {
            /* First use of an image we created: bring every mip into GENERAL. */
            VkImageMemoryBarrier init{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            init.srcAccessMask = 0;
            init.dstAccessMask = access(before) | access(after);
            init.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            init.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            init.srcQueueFamilyIndex = init.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            init.image = r->image;
            init.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stages(before) | stages(after),
                                 0, 0, nullptr, 0, nullptr, 1, &init);
            layout_flag(r) = true;
        }
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = access(before);
        b.dstAccessMask = access(after);
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = r->image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = mip == VK_REMAINING_MIP_LEVELS ? 0 : mip;
        b.subresourceRange.levelCount = mip == VK_REMAINING_MIP_LEVELS ? VK_REMAINING_MIP_LEVELS : 1;
        b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        vkCmdPipelineBarrier(cmd, stages(before), stages(after), 0, 0, nullptr, 0, nullptr, 1, &b);
    } else if (r->buffer) {
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask = access(before);
        b.dstAccessMask = access(after);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = r->buffer;
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, stages(before), stages(after), 0, 0, nullptr, 1, &b, 0, nullptr);
    }
    r->state = after;
}

/* Make an owned image usable before its first job if nothing transitioned it yet. */
static void ensure_layout(VkCommandBuffer cmd, Resource* r)
{
    if (r && r->image && !layout_flag(r))
        transition(cmd, r, r->state, r->state);
}

static VkImageView view(Backend* b, Resource* r, uint32_t mip)
{
    if (!b || !r || !r->image) return VK_NULL_HANDLE;
    const uint32_t mipCount = r->desc.mipCount ? r->desc.mipCount : 1;
    if (mip >= mipCount) mip = mipCount - 1;
    const VkFormat format = fsr3_vkpt_format_from_ffx((FfxApiSurfaceFormat)r->desc.format);
    const ViewKey key{r->image, format, mip};
    auto it = b->views.find(key);
    if (it != b->views.end()) return it->second;

    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = r->image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ci.subresourceRange.baseMipLevel = mip;
    ci.subresourceRange.levelCount = 1;
    ci.subresourceRange.layerCount = 1;
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(qvk.device, &ci, nullptr, &v) != VK_SUCCESS) {
        logf("FSR3: vkCreateImageView failed (format %d mip %u)", (int)format, mip);
        return VK_NULL_HANDLE;
    }
    b->views.emplace(key, v);
    return v;
}

static void drop_views(Backend* b, VkImage image)
{
    for (auto it = b->views.begin(); it != b->views.end();) {
        if (it->first.image == image) {
            vkDestroyImageView(qvk.device, it->second, nullptr);
            it = b->views.erase(it);
        } else {
            ++it;
        }
    }
}

static void destroy_resource_object(Backend* b, Resource* r)
{
    if (!r) return;
    if (b && r->image) drop_views(b, r->image);
    if (r->owned) {
        if (r->image) vkDestroyImage(qvk.device, r->image, nullptr);
        if (r->buffer) vkDestroyBuffer(qvk.device, r->buffer, nullptr);
        if (r->memory) vkFreeMemory(qvk.device, r->memory, nullptr);
    }
    delete r;
}

static FfxVersionNumber sdk(FfxInterface*) { return FFX_SDK_MAKE_VERSION(FFX_SDK_VERSION_MAJOR, FFX_SDK_VERSION_MINOR, FFX_SDK_VERSION_PATCH); }

static FfxErrorCode caps(FfxInterface*, FfxDeviceCapabilities* c)
{
    if (!c) return FFX_ERROR_INVALID_POINTER;
    std::memset(c, 0, sizeof(*c));
    /* The shaders are compiled once, for FP32 and without a wave-size
     * attribute (see fsr3_shader_manifest.cmake), so report capabilities that
     * make the SDK select exactly that permutation. */
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &subgroup;
    uint32_t lanes = 32;
    if (qvk.physical_device) {
        vkGetPhysicalDeviceProperties2(qvk.physical_device, &props);
        if (subgroup.subgroupSize) lanes = subgroup.subgroupSize;
    }
    c->maximumSupportedShaderModel = FFX_SHADER_MODEL_6_2;
    c->waveLaneCountMin = lanes;
    c->waveLaneCountMax = lanes;
    c->fp16Supported = false;
    c->dedicatedAllocationSupported = true;
    return FFX_OK;
}

static FfxErrorCode context_create(FfxInterface* i, FfxEffect, FfxEffectBindlessConfig*, FfxUInt32* id)
{
    if (!id) return FFX_ERROR_INVALID_POINTER;
    if (!backend(i)) return FFX_ERROR_NULL_DEVICE;
    *id = 0;
    return FFX_OK;
}

static FfxErrorCode context_destroy(FfxInterface*, FfxUInt32) { return FFX_OK; }

static FfxErrorCode memory_usage(FfxInterface* i, FfxUInt32, FfxApiEffectMemoryUsage* u)
{
    if (!u) return FFX_ERROR_INVALID_POINTER;
    Backend* b = backend(i);
    if (!b) return FFX_ERROR_NULL_DEVICE;
    u->totalUsageInBytes = u->aliasableUsageInBytes = 0;
    for (auto r : b->resources) {
        if (r && r->owned) {
            u->totalUsageInBytes += r->size;
            if (r->desc.flags & FFX_API_RESOURCE_FLAGS_ALIASABLE) u->aliasableUsageInBytes += r->size;
        }
    }
    return FFX_OK;
}

static FfxErrorCode register_resource(FfxInterface* i, const FfxApiResource* in, FfxUInt32, FfxResourceInternal* out)
{
    if (!in || !out || !in->resource) return FFX_ERROR_INVALID_POINTER;
    Backend* b = backend(i);
    if (!b) return FFX_ERROR_NULL_DEVICE;
    Resource* r = new (std::nothrow) Resource;
    if (!r) return FFX_ERROR_OUT_OF_MEMORY;
    r->desc = in->description;
    r->state = (FfxApiResourceState)in->state;
    r->transient = true;
    if (r->desc.type == FFX_API_RESOURCE_TYPE_BUFFER) r->buffer = (VkBuffer)in->resource;
    else r->image = (VkImage)in->resource;
    /* Shared resources (dilated depth/motion, reconstructed depth) come back
     * through here as plain handles; keep their layout state with the owner.
     * Anything else belongs to the renderer, which keeps it in GENERAL. */
    r->layoutReady = true;
    for (Resource* owned : b->resources) {
        if (owned && owned->owned && owned->image && owned->image == r->image) {
            r->alias = owned;
            break;
        }
    }
    b->resources.push_back(r);
    out->internalIndex = (int)b->resources.size() - 1;
    return FFX_OK;
}

/* Registered (non-owned) entries are always appended after the SDK's own
 * resources, so releasing them means popping the transient tail. */
static FfxErrorCode unregister_resources(FfxInterface* i, FfxCommandList, FfxUInt32)
{
    Backend* b = backend(i);
    if (!b) return FFX_ERROR_NULL_DEVICE;
    while (!b->resources.empty() && b->resources.back() && b->resources.back()->transient) {
        delete b->resources.back();
        b->resources.pop_back();
    }
    return FFX_OK;
}

static bool queue_upload(Backend* b, int resourceIndex, const FfxResourceInitData& init, VkDeviceSize resourceSize)
{
    if (init.type != FFX_RESOURCE_INIT_DATA_TYPE_BUFFER && init.type != FFX_RESOURCE_INIT_DATA_TYPE_VALUE) return true;
    VkDeviceSize size = init.size ? init.size : resourceSize;
    if (!size) return true;
    Upload up;
    up.resourceIndex = resourceIndex;
    up.size = size;
    if (buffer_create(&up.staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
        return false;
    void* mapped = buffer_map(&up.staging);
    if (!mapped) { buffer_destroy(&up.staging); return false; }
    if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER && init.buffer) std::memcpy(mapped, init.buffer, (size_t)size);
    else std::memset(mapped, init.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE ? init.value : 0, (size_t)size);
    buffer_unmap(&up.staging);
    b->pendingUploads.push_back(up);
    return true;
}

static FfxErrorCode create_resource(FfxInterface* i, const FfxCreateResourceDescription* d, FfxUInt32, FfxResourceInternal* out)
{
    if (!d || !out) return FFX_ERROR_INVALID_POINTER;
    Backend* b = backend(i);
    if (!b) return FFX_ERROR_NULL_DEVICE;
    Resource* r = new (std::nothrow) Resource;
    if (!r) return FFX_ERROR_OUT_OF_MEMORY;
    r->desc = d->resourceDescription;
    r->state = d->initialState;
    r->owned = true;
    VkResult vr = VK_SUCCESS;
    if (r->desc.type == FFX_API_RESOURCE_TYPE_BUFFER) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = r->desc.size;
        ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        vr = vkCreateBuffer(qvk.device, &ci, nullptr, &r->buffer);
        if (vr == VK_SUCCESS) {
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(qvk.device, r->buffer, &mr);
            r->size = mr.size;
            vr = allocate_gpu_memory(mr, &r->memory);
            if (vr == VK_SUCCESS) vr = vkBindBufferMemory(qvk.device, r->buffer, r->memory, 0);
        }
    } else {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fsr3_vkpt_format_from_ffx((FfxApiSurfaceFormat)r->desc.format);
        ci.extent = {r->desc.width, r->desc.height, 1};
        ci.mipLevels = r->desc.mipCount ? r->desc.mipCount : 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (ci.format == VK_FORMAT_UNDEFINED) vr = VK_ERROR_FORMAT_NOT_SUPPORTED;
        else vr = vkCreateImage(qvk.device, &ci, nullptr, &r->image);
        if (vr == VK_SUCCESS) {
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(qvk.device, r->image, &mr);
            r->size = mr.size;
            vr = allocate_gpu_memory(mr, &r->memory);
            if (vr == VK_SUCCESS) vr = vkBindImageMemory(qvk.device, r->image, r->memory, 0);
        }
    }
    if (vr != VK_SUCCESS) {
        logf("FSR3: resource creation failed (VkResult %d, type %u, format %u, %ux%u)", (int)vr,
             (unsigned)r->desc.type, (unsigned)r->desc.format, r->desc.width, r->desc.height);
        destroy_resource_object(b, r);
        return FFX_ERROR_BACKEND_API_ERROR;
    }
    b->resources.push_back(r);
    out->internalIndex = (int)b->resources.size() - 1;

    uint64_t dataSize = 0;
    if (GetResourceSizeFromDescription(nullptr, d, &dataSize, nullptr) != FFX_OK) dataSize = 0;
    if (!queue_upload(b, out->internalIndex, d->initData, dataSize)) {
        logf("FSR3: could not stage initial data for resource %d", out->internalIndex);
        return FFX_ERROR_BACKEND_API_ERROR;
    }
    return FFX_OK;
}

static FfxApiResource get_resource(FfxInterface* i, FfxResourceInternal h)
{
    FfxApiResource a{};
    Resource* r = resource(backend(i), h);
    if (r) {
        a.resource = r->buffer ? (void*)r->buffer : (void*)r->image;
        a.description = r->desc;
        a.state = r->state;
    }
    return a;
}

static FfxApiResourceDescription description(FfxInterface* i, FfxResourceInternal h)
{
    Resource* r = resource(backend(i), h);
    return r ? r->desc : FfxApiResourceDescription{};
}

static FfxErrorCode destroy_resource(FfxInterface* i, FfxResourceInternal h, FfxUInt32)
{
    Backend* b = backend(i);
    Resource* r = resource(b, h);
    if (!r) return FFX_ERROR_INVALID_ARGUMENT;
    destroy_resource_object(b, r);
    b->resources[h.internalIndex] = nullptr;
    return FFX_OK;
}

/* Constants are copied into a host-visible ring; the returned "data" pointer
 * carries the byte offset + 1 so that execute() can bind the right range. */
static FfxErrorCode stage(FfxInterface* i, void* data, FfxUInt32 size, FfxConstantBuffer* out)
{
    if (!data || !out || size > FFX_BUFFER_SIZE) return FFX_ERROR_INVALID_ARGUMENT;
    Backend* b = backend(i);
    if (!b) return FFX_ERROR_NULL_DEVICE;
    if (!b->constants.buffer &&
        buffer_create(&b->constants, FFX_CONSTANT_BUFFER_RING_BUFFER_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
        return FFX_ERROR_BACKEND_API_ERROR;
    if (!b->constantsMapped) b->constantsMapped = (char*)buffer_map(&b->constants);
    if (!b->constantsMapped) return FFX_ERROR_BACKEND_API_ERROR;
    const uint32_t align = (uint32_t)b->uboAlignment;
    uint32_t offset = (b->constantOffset + align - 1) & ~(align - 1);
    if (offset + size > b->constants.size) offset = 0;
    std::memcpy(b->constantsMapped + offset, data, size);
    b->constantOffset = offset + size;
    out->num32BitEntries = (size + 3) / 4;
    out->data = (uint32_t*)(b->constantsMapped + offset); // real host pointer into the ring
    return FFX_OK;
}

static void copy_name(wchar_t* dst, size_t dstCount, const char* src)
{
    if (!dst || !dstCount) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = std::mbstowcs(dst, src, dstCount - 1);
    if (n == (size_t)-1) n = 0;
    dst[n] = 0;
}

static bool sampler_is_point(const char* name, const FfxSamplerDescription* fallback)
{
    if (name && (std::strstr(name, "Point") || std::strstr(name, "point") || std::strstr(name, "Nearest")))
        return true;
    if (name && (std::strstr(name, "Linear") || std::strstr(name, "linear")))
        return false;
    return fallback && fallback->filter == FFX_FILTER_TYPE_MINMAGMIP_POINT;
}

static FfxErrorCode create_pipeline(FfxInterface* i, FfxShaderBlob* blob, const FfxPipelineDescription* d, FfxUInt32, FfxPipelineState* out)
{
    if (!blob || !d || !out) return FFX_ERROR_INVALID_POINTER;
    if (!blob->data || !blob->size) {
        logf("FSR3: no shader permutation available for this pass (flags 0x%x)", d->contextFlags);
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    if (d->indirectWorkload) return unsupported();
    Backend* b = backend(i);
    if (!b) return FFX_ERROR_NULL_DEVICE;
    Pipeline* p = new (std::nothrow) Pipeline;
    if (!p) return FFX_ERROR_OUT_OF_MEMORY;

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    auto add = [&](uint32_t slot, uint32_t count, VkDescriptorType type) {
        for (auto& existing : bindings) {
            if (existing.binding == slot) {
                if (existing.descriptorType != type)
                    logf("FSR3: binding %u declared twice with different types", slot);
                return;
            }
        }
        bindings.push_back({slot, type, count ? count : 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    };
    for (uint32_t x = 0; x < blob->srvTextureCount; x++)
        add(blob->boundSRVTextures[x], blob->boundSRVTextureCounts ? blob->boundSRVTextureCounts[x] : 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    for (uint32_t x = 0; x < blob->uavTextureCount; x++)
        add(blob->boundUAVTextures[x], blob->boundUAVTextureCounts ? blob->boundUAVTextureCounts[x] : 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    for (uint32_t x = 0; x < blob->srvBufferCount; x++)
        add(blob->boundSRVBuffers[x], blob->boundSRVBufferCounts ? blob->boundSRVBufferCounts[x] : 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    for (uint32_t x = 0; x < blob->uavBufferCount; x++)
        add(blob->boundUAVBuffers[x], blob->boundUAVBufferCounts ? blob->boundUAVBufferCounts[x] : 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    for (uint32_t x = 0; x < blob->cbvCount; x++)
        add(blob->boundConstantBuffers[x], blob->boundConstantBufferCounts ? blob->boundConstantBufferCounts[x] : 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    for (uint32_t x = 0; x < blob->samplerCount; x++)
        add(blob->boundSamplers[x], blob->boundSamplerCounts ? blob->boundSamplerCounts[x] : 1, VK_DESCRIPTOR_TYPE_SAMPLER);

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = (uint32_t)bindings.size();
    li.pBindings = bindings.data();
    VkResult vr = vkCreateDescriptorSetLayout(qvk.device, &li, nullptr, &p->descriptors);
    if (vr != VK_SUCCESS) {
        logf("FSR3: vkCreateDescriptorSetLayout failed (%d)", (int)vr);
        delete p;
        return FFX_ERROR_BACKEND_API_ERROR;
    }

    /* The SDK's "root constants" are the same cbuffers that stage() streams;
     * they are bound as uniform buffers, so the layout has no push constants. */
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &p->descriptors;
    vr = vkCreatePipelineLayout(qvk.device, &pli, nullptr, &p->layout);
    if (vr != VK_SUCCESS) {
        logf("FSR3: vkCreatePipelineLayout failed (%d)", (int)vr);
        vkDestroyDescriptorSetLayout(qvk.device, p->descriptors, nullptr);
        delete p;
        return FFX_ERROR_BACKEND_API_ERROR;
    }

    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = blob->size;
    si.pCode = (const uint32_t*)blob->data;
    VkShaderModule sm = VK_NULL_HANDLE;
    vr = vkCreateShaderModule(qvk.device, &si, nullptr, &sm);
    if (vr == VK_SUCCESS) {
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pi.stage.module = sm;
        pi.stage.pName = blob->entryName ? blob->entryName : "main";
        pi.layout = p->layout;
        vr = vkCreateComputePipelines(qvk.device, VK_NULL_HANDLE, 1, &pi, nullptr, &p->pipeline);
        vkDestroyShaderModule(qvk.device, sm, nullptr);
    }
    if (vr != VK_SUCCESS) {
        logf("FSR3: compute pipeline creation failed (%d)", (int)vr);
        vkDestroyPipelineLayout(qvk.device, p->layout, nullptr);
        vkDestroyDescriptorSetLayout(qvk.device, p->descriptors, nullptr);
        delete p;
        return FFX_ERROR_BACKEND_API_ERROR;
    }

    for (uint32_t x = 0; x < blob->samplerCount; x++) {
        const char* name = blob->boundSamplerNames ? blob->boundSamplerNames[x] : nullptr;
        const FfxSamplerDescription* fallback = (d->samplers && x < d->samplerCount) ? &d->samplers[x] : nullptr;
        const bool point = sampler_is_point(name, fallback);
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sci.mipmapMode = point ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(qvk.device, &sci, nullptr, &sampler) != VK_SUCCESS) {
            logf("FSR3: vkCreateSampler failed");
            break;
        }
        p->samplers.push_back(sampler);
        p->samplerSlots.push_back(blob->boundSamplers[x]);
    }

    b->pipelines.push_back(p);
    std::memset(out, 0, sizeof(*out));
    out->pipeline = p;
    out->rootSignature = p->descriptors;
    out->uavTextureCount = blob->uavTextureCount;
    out->srvTextureCount = blob->srvTextureCount;
    out->srvBufferCount = blob->srvBufferCount;
    out->uavBufferCount = blob->uavBufferCount;
    out->constCount = blob->cbvCount;
    const size_t nameCount = sizeof(out->srvTextureBindings[0].name) / sizeof(wchar_t);
    for (uint32_t x = 0; x < blob->uavTextureCount; x++) {
        out->uavTextureBindings[x].slotIndex = blob->boundUAVTextures[x];
        copy_name(out->uavTextureBindings[x].name, nameCount, blob->boundUAVTextureNames ? blob->boundUAVTextureNames[x] : nullptr);
    }
    for (uint32_t x = 0; x < blob->srvTextureCount; x++) {
        out->srvTextureBindings[x].slotIndex = blob->boundSRVTextures[x];
        copy_name(out->srvTextureBindings[x].name, nameCount, blob->boundSRVTextureNames ? blob->boundSRVTextureNames[x] : nullptr);
    }
    for (uint32_t x = 0; x < blob->srvBufferCount; x++) {
        out->srvBufferBindings[x].slotIndex = blob->boundSRVBuffers[x];
        copy_name(out->srvBufferBindings[x].name, nameCount, blob->boundSRVBufferNames ? blob->boundSRVBufferNames[x] : nullptr);
    }
    for (uint32_t x = 0; x < blob->uavBufferCount; x++) {
        out->uavBufferBindings[x].slotIndex = blob->boundUAVBuffers[x];
        copy_name(out->uavBufferBindings[x].name, nameCount, blob->boundUAVBufferNames ? blob->boundUAVBufferNames[x] : nullptr);
    }
    for (uint32_t x = 0; x < blob->cbvCount; x++) {
        out->constantBufferBindings[x].slotIndex = blob->boundConstantBuffers[x];
        out->constantBufferBindings[x].arrayIndex = 1;
        copy_name(out->constantBufferBindings[x].name, nameCount, blob->boundConstantBufferNames ? blob->boundConstantBufferNames[x] : nullptr);
    }
    return FFX_OK;
}

static void destroy_pipeline_object(Pipeline* p)
{
    if (!p) return;
    for (VkSampler s : p->samplers) if (s) vkDestroySampler(qvk.device, s, nullptr);
    if (p->pipeline) vkDestroyPipeline(qvk.device, p->pipeline, nullptr);
    if (p->layout) vkDestroyPipelineLayout(qvk.device, p->layout, nullptr);
    if (p->descriptors) vkDestroyDescriptorSetLayout(qvk.device, p->descriptors, nullptr);
    delete p;
}

static FfxErrorCode destroy_pipeline(FfxInterface* i, FfxPipelineState* s, FfxUInt32)
{
    if (!s || !s->pipeline) return FFX_ERROR_INVALID_POINTER;
    Pipeline* p = (Pipeline*)s->pipeline;
    Backend* b = backend(i);
    if (b) {
        auto it = std::find(b->pipelines.begin(), b->pipelines.end(), p);
        if (it != b->pipelines.end()) b->pipelines.erase(it);
    }
    destroy_pipeline_object(p);
    s->pipeline = nullptr;
    return FFX_OK;
}

static FfxErrorCode schedule(FfxInterface* i, const FfxGpuJobDescription* j)
{
    if (!j) return FFX_ERROR_INVALID_POINTER;
    Backend* b = backend(i);
    if (!b) return FFX_ERROR_NULL_DEVICE;
    if (b->jobs.size() >= FFX_MAX_GPU_JOBS) return FFX_ERROR_OUT_OF_RANGE;
    b->jobs.push_back({*j});
    return FFX_OK;
}

static FfxErrorCode query(FfxInterface* i, FfxGpuJobDescription** out)
{
    if (!out) return FFX_ERROR_INVALID_POINTER;
    Backend* b = backend(i);
    if (!b) return FFX_ERROR_NULL_DEVICE;
    if (b->jobs.empty()) { *out = nullptr; return FFX_ERROR_EOF; }
    *out = &b->jobs.front().job;
    return FFX_OK;
}

static VkResult create_descriptor_pools(Backend* b)
{
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 256},
    };
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = 128;
    info.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
    info.pPoolSizes = sizes;
    for (uint32_t f = 0; f < kQueuedFrames; f++) {
        VkResult vr = vkCreateDescriptorPool(qvk.device, &info, nullptr, &b->pools[f]);
        if (vr != VK_SUCCESS) return vr;
    }
    return VK_SUCCESS;
}

static void record_uploads(Backend* b, VkCommandBuffer cmd)
{
    for (Upload& up : b->pendingUploads) {
        Resource* r = (up.resourceIndex >= 0 && (size_t)up.resourceIndex < b->resources.size()) ? b->resources[up.resourceIndex] : nullptr;
        if (r) {
            FfxApiResourceState finalState = r->state;
            transition(cmd, r, r->state, FFX_API_RESOURCE_STATE_COPY_DEST);
            if (r->image) {
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {r->desc.width, r->desc.height, 1};
                vkCmdCopyBufferToImage(cmd, up.staging.buffer, r->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            } else if (r->buffer) {
                VkBufferCopy region{0, 0, std::min<VkDeviceSize>(up.size, r->desc.size ? r->desc.size : up.size)};
                vkCmdCopyBuffer(cmd, up.staging.buffer, r->buffer, 1, &region);
            }
            transition(cmd, r, FFX_API_RESOURCE_STATE_COPY_DEST, finalState);
        }
        /* The staging buffer must outlive the command buffer; it is released
         * with the interface. */
        b->retiredStaging.push_back(up.staging);
    }
    b->pendingUploads.clear();
}

static FfxErrorCode execute_compute(Backend* b, VkCommandBuffer cmd, const FfxComputeJobDescription& c)
{
    Pipeline* p = c.pipeline ? (Pipeline*)c.pipeline->pipeline : nullptr;
    if (!p) return FFX_ERROR_INVALID_ARGUMENT;

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = b->pools[b->poolIndex];
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p->descriptors;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult vr = vkAllocateDescriptorSets(qvk.device, &ai, &set);
    if (vr != VK_SUCCESS) {
        logf("FSR3: vkAllocateDescriptorSets failed (%d)", (int)vr);
        return FFX_ERROR_BACKEND_API_ERROR;
    }

    const size_t imageCount = c.pipeline->srvTextureCount + c.pipeline->uavTextureCount + p->samplers.size();
    const size_t bufferCount = c.pipeline->srvBufferCount + c.pipeline->uavBufferCount + c.pipeline->constCount;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkWriteDescriptorSet> writes;
    images.reserve(imageCount);
    buffers.reserve(bufferCount);
    writes.reserve(imageCount + bufferCount);

    auto write_image = [&](uint32_t slot, VkDescriptorType type, VkImageView v, VkSampler s) {
        images.push_back({s, v, VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.dstBinding = slot; w.descriptorCount = 1; w.descriptorType = type;
        w.pImageInfo = &images.back();
        writes.push_back(w);
    };
    auto write_buffer = [&](uint32_t slot, VkDescriptorType type, VkBuffer buf, VkDeviceSize offset, VkDeviceSize range) {
        buffers.push_back({buf, offset, range});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.dstBinding = slot; w.descriptorCount = 1; w.descriptorType = type;
        w.pBufferInfo = &buffers.back();
        writes.push_back(w);
    };

    for (uint32_t x = 0; x < c.pipeline->srvTextureCount; x++) {
        Resource* r = resource(b, c.srvTextures[x].resource);
        if (!r) continue;
        ensure_layout(cmd, r);
        write_image(c.pipeline->srvTextureBindings[x].slotIndex, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view(b, r, 0), VK_NULL_HANDLE);
    }
    for (uint32_t x = 0; x < c.pipeline->uavTextureCount; x++) {
        Resource* r = resource(b, c.uavTextures[x].resource);
        if (!r) continue;
        ensure_layout(cmd, r);
        write_image(c.pipeline->uavTextureBindings[x].slotIndex, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view(b, r, c.uavTextures[x].mip), VK_NULL_HANDLE);
    }
    for (size_t x = 0; x < p->samplers.size(); x++)
        write_image(p->samplerSlots[x], VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, p->samplers[x]);
    for (uint32_t x = 0; x < c.pipeline->srvBufferCount; x++) {
        Resource* r = resource(b, c.srvBuffers[x].resource);
        if (!r || !r->buffer) continue;
        write_buffer(c.pipeline->srvBufferBindings[x].slotIndex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, r->buffer,
                     c.srvBuffers[x].offset, c.srvBuffers[x].size ? c.srvBuffers[x].size : VK_WHOLE_SIZE);
    }
    for (uint32_t x = 0; x < c.pipeline->uavBufferCount; x++) {
        Resource* r = resource(b, c.uavBuffers[x].resource);
        if (!r || !r->buffer) continue;
        write_buffer(c.pipeline->uavBufferBindings[x].slotIndex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, r->buffer,
                     c.uavBuffers[x].offset, c.uavBuffers[x].size ? c.uavBuffers[x].size : VK_WHOLE_SIZE);
    }
    for (uint32_t x = 0; x < c.pipeline->constCount && x < FFX_MAX_NUM_CONST_BUFFERS; x++) {
        const FfxConstantBuffer& cb = c.cbs[x];
        if (!cb.data || !b->constants.buffer) {
            logf("FSR3: constant buffer %u was not staged", x);
            return FFX_ERROR_INVALID_ARGUMENT;
        }
        const char* p_data = (const char*)cb.data;
        if (!b->constantsMapped || p_data < b->constantsMapped || p_data >= b->constantsMapped + b->constants.size) {
            logf("FSR3: constant buffer %u points outside the staging ring", x);
            return FFX_ERROR_INVALID_ARGUMENT;
        }
        const VkDeviceSize offset = (VkDeviceSize)(p_data - b->constantsMapped);
        write_buffer(c.pipeline->constantBufferBindings[x].slotIndex, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b->constants.buffer,
                     offset, (VkDeviceSize)cb.num32BitEntries * 4);
    }

    vkUpdateDescriptorSets(qvk.device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, c.dimensions[0], c.dimensions[1], c.dimensions[2]);

    /* Storage writes of this pass must be visible to whatever the SDK
     * schedules next, whether or not it emits an explicit barrier job. */
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    return FFX_OK;
}

static FfxErrorCode execute(FfxInterface* i, FfxCommandList cl, FfxUInt32)
{
    if (!cl) return FFX_ERROR_INVALID_POINTER;
    Backend* b = backend(i);
    if (!b) return FFX_ERROR_NULL_DEVICE;
    VkCommandBuffer cmd = (VkCommandBuffer)cl;
    FfxErrorCode result = FFX_OK;

    record_uploads(b, cmd);

    for (auto& queued : b->jobs) {
        auto& j = queued.job;
        if (j.jobType == FFX_GPU_JOB_BARRIER) {
            auto& x = j.barrierDescriptor;
            transition(cmd, resource(b, x.resource), x.currentState, x.newState, x.subResourceID);
        } else if (j.jobType == FFX_GPU_JOB_CLEAR_FLOAT) {
            Resource* r = resource(b, j.clearJobDescriptor.target);
            if (!r || !r->image) { result = FFX_ERROR_INVALID_ARGUMENT; break; }
            const FfxApiResourceState previous = r->state;
            transition(cmd, r, r->state, FFX_API_RESOURCE_STATE_COPY_DEST);
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            VkClearColorValue color{};
            std::memcpy(&color, j.clearJobDescriptor.color, sizeof(color));
            vkCmdClearColorImage(cmd, r->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
            transition(cmd, r, FFX_API_RESOURCE_STATE_COPY_DEST, previous);
        } else if (j.jobType == FFX_GPU_JOB_COPY) {
            Resource* a = resource(b, j.copyJobDescriptor.src);
            Resource* d = resource(b, j.copyJobDescriptor.dst);
            if (!a || !d) { result = FFX_ERROR_INVALID_ARGUMENT; break; }
            if (a->buffer && d->buffer) {
                VkDeviceSize n = j.copyJobDescriptor.size ? j.copyJobDescriptor.size : std::min(a->size, d->size);
                VkBufferCopy region{j.copyJobDescriptor.srcOffset, j.copyJobDescriptor.dstOffset, n};
                vkCmdCopyBuffer(cmd, a->buffer, d->buffer, 1, &region);
            } else if (a->image && d->image) {
                ensure_layout(cmd, a);
                ensure_layout(cmd, d);
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {std::min(a->desc.width, d->desc.width), std::min(a->desc.height, d->desc.height), 1};
                vkCmdCopyImage(cmd, a->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            } else {
                result = unsupported();
                break;
            }
        } else if (j.jobType == FFX_GPU_JOB_COMPUTE) {
            result = execute_compute(b, cmd, j.computeJobDescriptor);
            if (result != FFX_OK) break;
        } else if (j.jobType == FFX_GPU_JOB_DISCARD) {
            /* The SDK is telling us the previous contents are dead.  With one
             * layout for everything there is nothing to record. */
        } else {
            /* The SDK ignores this function's return code, so skipping an
             * unknown job is safer than dropping every job after it. */
            static int reported = 0;
            if (reported++ < 4) logf("FSR3: unsupported GPU job type %d skipped", (int)j.jobType);
        }
    }
    b->jobs.clear();
    return result;
}

static FfxErrorCode no_static(FfxInterface*, const FfxStaticResourceDescription*, FfxUInt32) { return unsupported(); }
static FfxErrorCode no_map(FfxInterface*, FfxResourceInternal, void**) { return unsupported(); }
static FfxErrorCode no_unmap(FfxInterface*, FfxResourceInternal) { return unsupported(); }
static FfxErrorCode no_heap_create(FfxInterface*, const FfxCreateHeapDescription*, FfxUInt32, FfxResourceHeap*) { return unsupported(); }
static FfxErrorCode no_heap_destroy(FfxInterface*, FfxResourceHeap, FfxUInt32) { return unsupported(); }
static FfxErrorCode no_framegen(FfxFrameGenerationConfig const*) { return unsupported(); }
static FfxABIVersion no_swapchain_abi(FfxSwapchain) { return FFX_ABI_INVALID; }

static FfxApiSurfaceFormat format_from_vk(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R32G32B32A32_UINT: return FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32G32B32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32G32_UINT: return FFX_API_SURFACE_FORMAT_R32G32_UINT;
    case VK_FORMAT_R8_UINT: return FFX_API_SURFACE_FORMAT_R8_UINT;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case VK_FORMAT_R8_SNORM: return FFX_API_SURFACE_FORMAT_R8_SNORM;
    case VK_FORMAT_R32_UINT: return FFX_API_SURFACE_FORMAT_R32_UINT;
    case VK_FORMAT_R32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16G16_UINT: return FFX_API_SURFACE_FORMAT_R16G16_UINT;
    case VK_FORMAT_R16G16_SINT: return FFX_API_SURFACE_FORMAT_R16G16_SINT;
    case VK_FORMAT_R16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16_FLOAT;
    case VK_FORMAT_R16_UINT: return FFX_API_SURFACE_FORMAT_R16_UINT;
    case VK_FORMAT_R16_UNORM: return FFX_API_SURFACE_FORMAT_R16_UNORM;
    case VK_FORMAT_R16_SNORM: return FFX_API_SURFACE_FORMAT_R16_SNORM;
    case VK_FORMAT_R8G8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8_UINT: return FFX_API_SURFACE_FORMAT_R8G8_UINT;
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

static VkFormat format_to_vk(FfxApiSurfaceFormat f)
{
    switch (f) {
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32_TYPELESS: return VK_FORMAT_R32G32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_UINT;
    case FFX_API_SURFACE_FORMAT_R8_TYPELESS: return VK_FORMAT_R8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    case FFX_API_SURFACE_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8_SNORM: return VK_FORMAT_R8_SNORM;
    case FFX_API_SURFACE_FORMAT_R32_TYPELESS: return VK_FORMAT_R32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case FFX_API_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS: return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS: return VK_FORMAT_B8G8R8A8_UNORM;
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case FFX_API_SURFACE_FORMAT_R16G16_TYPELESS: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UINT;
    case FFX_API_SURFACE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case FFX_API_SURFACE_FORMAT_R16_TYPELESS: return VK_FORMAT_R16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
    case FFX_API_SURFACE_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
    case FFX_API_SURFACE_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
    case FFX_API_SURFACE_FORMAT_R8G8_TYPELESS: return VK_FORMAT_R8G8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_UINT;
    case FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

} // namespace

FfxErrorCode GetResourceSizeFromDescription(FfxDevice, const FfxCreateResourceDescription* d,
                                             uint64_t* size, uint64_t* alignment)
{
    if (!d || !size)
        return FFX_ERROR_INVALID_POINTER;
    const FfxApiResourceDescription& r = d->resourceDescription;
    if (r.type == FFX_API_RESOURCE_TYPE_BUFFER) {
        *size = r.size;
    } else {
        uint32_t bytes = 0;
        switch ((FfxApiSurfaceFormat)r.format) {
        case FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS: bytes = 16; break;
        case FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT: bytes = 16; break;
        case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT: bytes = 16; break;
        case FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS: bytes = 8; break;
        case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT: bytes = 8; break;
        case FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT: bytes = 12; break;
        case FFX_API_SURFACE_FORMAT_R32G32_TYPELESS: bytes = 8; break;
        case FFX_API_SURFACE_FORMAT_R32G32_FLOAT: bytes = 8; break;
        case FFX_API_SURFACE_FORMAT_R32G32_UINT: bytes = 8; break;
        case FFX_API_SURFACE_FORMAT_R8_TYPELESS: bytes = 1; break;
        case FFX_API_SURFACE_FORMAT_R8_UINT: bytes = 1; break;
        case FFX_API_SURFACE_FORMAT_R8_UNORM: bytes = 1; break;
        case FFX_API_SURFACE_FORMAT_R8_SNORM: bytes = 1; break;
        case FFX_API_SURFACE_FORMAT_R32_TYPELESS: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R32_UINT: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R32_FLOAT: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R16G16_TYPELESS: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R16G16_FLOAT: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R16G16_UINT: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R16G16_SINT: bytes = 4; break;
        case FFX_API_SURFACE_FORMAT_R16_TYPELESS: bytes = 2; break;
        case FFX_API_SURFACE_FORMAT_R16_FLOAT: bytes = 2; break;
        case FFX_API_SURFACE_FORMAT_R16_UINT: bytes = 2; break;
        case FFX_API_SURFACE_FORMAT_R16_UNORM: bytes = 2; break;
        case FFX_API_SURFACE_FORMAT_R16_SNORM: bytes = 2; break;
        case FFX_API_SURFACE_FORMAT_R8G8_TYPELESS: bytes = 2; break;
        case FFX_API_SURFACE_FORMAT_R8G8_UNORM: bytes = 2; break;
        case FFX_API_SURFACE_FORMAT_R8G8_UINT: bytes = 2; break;
        case FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP: bytes = 4; break;
        default: return FFX_ERROR_INVALID_ENUM;
        }
        *size = (uint64_t)r.width * r.height * (r.depth ? r.depth : 1) * bytes;
    }
    if (alignment)
        *alignment = 256;
    return FFX_OK;
}

extern "C" bool fsr3_vkpt_is_available(void) { return qvk.device_count == 1 && qvk.device != VK_NULL_HANDLE; }
extern "C" FfxApiSurfaceFormat fsr3_vkpt_format_to_ffx(VkFormat f) { return format_from_vk(f); }
extern "C" VkFormat fsr3_vkpt_format_from_ffx(FfxApiSurfaceFormat f) { return format_to_vk(f); }

extern "C" FfxApiResource fsr3_vkpt_wrap_image(VkImage image, const FfxApiResourceDescription* d, FfxApiResourceState s)
{
    FfxApiResource a{};
    a.resource = (void*)image;
    if (d) a.description = *d;
    a.state = s;
    return a;
}

extern "C" FfxApiResource fsr3_vkpt_wrap_buffer(VkBuffer buffer, const FfxApiResourceDescription* d, FfxApiResourceState s)
{
    FfxApiResource a{};
    a.resource = (void*)buffer;
    if (d) a.description = *d;
    a.state = s;
    return a;
}

extern "C" void fsr3_vkpt_begin_frame(FfxInterface* i, uint32_t frameIndex)
{
    Backend* b = backend(i);
    if (!b) return;
    b->poolIndex = frameIndex % kQueuedFrames;
    if (b->pools[b->poolIndex]) vkResetDescriptorPool(qvk.device, b->pools[b->poolIndex], 0);
}

extern "C" FfxErrorCode fsr3_vkpt_create_interface(FfxInterface* out)
{
    if (!out) return FFX_ERROR_INVALID_POINTER;
    if (!fsr3_vkpt_is_available()) return FFX_ERROR_INVALID_ARGUMENT;
    Backend* b = new (std::nothrow) Backend;
    if (!b) return FFX_ERROR_OUT_OF_MEMORY;
    if (create_descriptor_pools(b) != VK_SUCCESS) {
        for (auto pool : b->pools) if (pool) vkDestroyDescriptorPool(qvk.device, pool, nullptr);
        delete b;
        return FFX_ERROR_BACKEND_API_ERROR;
    }
    if (qvk.physical_device) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(qvk.physical_device, &props);
        b->uboAlignment = std::max<VkDeviceSize>(256, props.limits.minUniformBufferOffsetAlignment);
    }
    std::memset(out, 0, sizeof(*out));
    out->scratchBuffer = b;
    out->scratchBufferSize = sizeof(*b);
    out->device = (FfxDevice)qvk.device;
    out->fpGetSDKVersion = sdk;
    out->fpGetEffectGpuMemoryUsage = memory_usage;
    out->fpCreateBackendContext = context_create;
    out->fpGetDeviceCapabilities = caps;
    out->fpDestroyBackendContext = context_destroy;
    out->fpCreateResource = create_resource;
    out->fpRegisterResource = register_resource;
    out->fpGetResource = get_resource;
    out->fpUnregisterResources = unregister_resources;
    out->fpRegisterStaticResource = no_static;
    out->fpGetResourceDescription = description;
    out->fpDestroyResource = destroy_resource;
    out->fpMapResource = no_map;
    out->fpUnmapResource = no_unmap;
    out->fpStageConstantBufferDataFunc = stage;
    out->fpCreatePipeline = create_pipeline;
    out->fpDestroyPipeline = destroy_pipeline;
    out->fpScheduleGpuJob = schedule;
    out->fpQueryNextGpuJobDesc = query;
    out->fpExecuteGpuJobs = execute;
    out->fpSwapChainConfigureFrameGeneration = no_framegen;
    out->fpGetSwapchainABI = no_swapchain_abi;
    out->fpCreateHeap = no_heap_create;
    out->fpDestroyHeap = no_heap_destroy;
    return FFX_OK;
}

extern "C" void fsr3_vkpt_destroy_interface(FfxInterface* i)
{
    if (!i) return;
    Backend* b = backend(i);
    if (!b) return;
    for (auto p : b->pipelines) destroy_pipeline_object(p);
    for (auto r : b->resources) destroy_resource_object(b, r);
    for (auto& kv : b->views) vkDestroyImageView(qvk.device, kv.second, nullptr);
    for (auto pool : b->pools) if (pool) vkDestroyDescriptorPool(qvk.device, pool, nullptr);
    for (auto& up : b->pendingUploads) buffer_destroy(&up.staging);
    for (auto& staging : b->retiredStaging) buffer_destroy(&staging);
    if (b->constants.buffer) { if (b->constantsMapped) buffer_unmap(&b->constants); buffer_destroy(&b->constants); }
    delete b;
    std::memset(i, 0, sizeof(*i));
}
