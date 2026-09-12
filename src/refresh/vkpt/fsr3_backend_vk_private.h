#pragma once

#include <vulkan/vulkan.h>

#include "vk_util.h"

/* The backend only needs these leading QVK fields.  Keep this declaration
 * private so the FSR3 unit does not depend on the renderer's C++-hostile
 * umbrella header. */
typedef struct Fsr3QvkPrefix {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkPhysicalDeviceMemoryProperties mem_properties;
    int device_count;
#ifdef VKPT_DEVICE_GROUPS
    VkPhysicalDevice device_group_physical_devices[VKPT_MAX_GPUS];
#endif
    VkDevice device;
} Fsr3QvkPrefix;

#ifdef __cplusplus
extern "C" {
#endif
extern Fsr3QvkPrefix qvk;
VkResult allocate_gpu_memory(VkMemoryRequirements requirements, VkDeviceMemory* memory);
#ifdef __cplusplus
}
#endif
