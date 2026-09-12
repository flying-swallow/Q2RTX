#include "fsr3_backend_vk.h"
#include "fsr3_backend_vk_private.h"

#include <cassert>
#include <cstdint>

extern "C" {
Fsr3QvkPrefix qvk{};
VkResult allocate_gpu_memory(VkMemoryRequirements, VkDeviceMemory*) { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult buffer_create(BufferResource_t*, VkDeviceSize, VkBufferUsageFlags, VkMemoryPropertyFlags) { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult buffer_destroy(BufferResource_t*) { return VK_SUCCESS; }
void* buffer_map(BufferResource_t*) { return nullptr; }
void buffer_unmap(BufferResource_t*) {}
void fsr3_vkpt_log(const char*) {}
}

namespace {

void check_interface_callbacks(const FfxInterface& backend)
{
    assert(backend.fpGetSDKVersion);
    assert(backend.fpGetEffectGpuMemoryUsage);
    assert(backend.fpCreateBackendContext);
    assert(backend.fpGetDeviceCapabilities);
    assert(backend.fpDestroyBackendContext);
    assert(backend.fpCreateResource);
    assert(backend.fpRegisterResource);
    assert(backend.fpGetResource);
    assert(backend.fpUnregisterResources);
    assert(backend.fpRegisterStaticResource);
    assert(backend.fpGetResourceDescription);
    assert(backend.fpDestroyResource);
    assert(backend.fpMapResource);
    assert(backend.fpUnmapResource);
    assert(backend.fpStageConstantBufferDataFunc);
    assert(backend.fpCreatePipeline);
    assert(backend.fpDestroyPipeline);
    assert(backend.fpScheduleGpuJob);
    assert(backend.fpExecuteGpuJobs);
    assert(backend.fpSwapChainConfigureFrameGeneration);
    assert(backend.fpGetSwapchainABI);
    assert(backend.fpCreateHeap);
    assert(backend.fpDestroyHeap);
    assert(backend.fpQueryNextGpuJobDesc);
}

void check_resource_conversions()
{
    FfxApiResourceDescription description{};
    description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    description.format = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    description.width = 1920;
    description.height = 1080;
    description.mipCount = 1;

    const FfxApiResource image = fsr3_vkpt_wrap_image(
        reinterpret_cast<VkImage>(static_cast<uintptr_t>(0x1234)), &description,
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    assert(image.resource == reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234)));
    assert(image.description.type == description.type);
    assert(image.description.format == description.format);
    assert(image.description.width == description.width);
    assert(image.description.height == description.height);
    assert(image.state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    description.type = FFX_API_RESOURCE_TYPE_BUFFER;
    description.size = 4096;
    const FfxApiResource buffer = fsr3_vkpt_wrap_buffer(
        reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(0x5678)), &description,
        FFX_API_RESOURCE_STATE_COPY_SRC);
    assert(buffer.resource == reinterpret_cast<void*>(static_cast<uintptr_t>(0x5678)));
    assert(buffer.description.type == FFX_API_RESOURCE_TYPE_BUFFER);
    assert(buffer.description.size == 4096);
    assert(buffer.state == FFX_API_RESOURCE_STATE_COPY_SRC);
}

void check_format_conversions()
{
    assert(fsr3_vkpt_format_to_ffx(VK_FORMAT_R16G16B16A16_SFLOAT) ==
           FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT);
    assert(fsr3_vkpt_format_to_ffx(VK_FORMAT_R32_SFLOAT) == FFX_API_SURFACE_FORMAT_R32_FLOAT);
    assert(fsr3_vkpt_format_to_ffx(VK_FORMAT_R8_UNORM) == FFX_API_SURFACE_FORMAT_R8_UNORM);
    assert(fsr3_vkpt_format_from_ffx(FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT) ==
           VK_FORMAT_R16G16B16A16_SFLOAT);
    assert(fsr3_vkpt_format_from_ffx(FFX_API_SURFACE_FORMAT_R32_UINT) == VK_FORMAT_R32_UINT);
    assert(fsr3_vkpt_format_to_ffx(VK_FORMAT_UNDEFINED) == FFX_API_SURFACE_FORMAT_UNKNOWN);
    assert(fsr3_vkpt_format_from_ffx(FFX_API_SURFACE_FORMAT_UNKNOWN) == VK_FORMAT_UNDEFINED);
}

} // namespace

int main()
{
    check_format_conversions();
    check_resource_conversions();

    FfxInterface backend{};
    const FfxErrorCode result = fsr3_vkpt_create_interface(&backend);
    if (result != FFX_OK) {
        // A headless build has no renderer-owned VkDevice. Conversion and wrapper
        // checks above remain useful, while interface creation is tested when one
        // is available.
        assert(!fsr3_vkpt_is_available());
        return 0;
    }

    check_interface_callbacks(backend);
    assert(backend.fpCreateResource(nullptr, nullptr, 0, nullptr) != FFX_OK);
    assert(backend.fpScheduleGpuJob(&backend, nullptr) != FFX_OK);
    fsr3_vkpt_destroy_interface(&backend);
    return 0;
}
