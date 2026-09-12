#pragma once

#include <vulkan/vulkan.h>

#if !defined(_WIN32) && !defined(__declspec)
#define __declspec(x)
#define FSR3_VKPT_UNDEF_DECLSPEC
#endif
#include "../../../extern/FidelityFX-SDK/Kits/FidelityFX/api/internal/ffx_interface.h"
#ifdef FSR3_VKPT_UNDEF_DECLSPEC
#undef __declspec
#undef FSR3_VKPT_UNDEF_DECLSPEC
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The backend uses the renderer's already-created device and graphics command
 * queue.  The returned interface owns its callback state and must be released
 * with fsr3_vkpt_destroy_interface(). */
bool fsr3_vkpt_is_available(void);
FfxErrorCode fsr3_vkpt_create_interface(FfxInterface* outInterface);
void fsr3_vkpt_destroy_interface(FfxInterface* interface);

FfxApiSurfaceFormat fsr3_vkpt_format_to_ffx(VkFormat format);
VkFormat fsr3_vkpt_format_from_ffx(FfxApiSurfaceFormat format);

/* Call once per frame before dispatching: selects and resets the descriptor
 * pool for that frame in flight. */
void fsr3_vkpt_begin_frame(FfxInterface* interface, uint32_t frameIndex);

/* Provided by the renderer (fsr.c); the backend reports failures through it. */
void fsr3_vkpt_log(const char* message);

/* Helpers for importing renderer-owned images and buffers into FSR3. */
FfxApiResource fsr3_vkpt_wrap_image(VkImage image, const FfxApiResourceDescription* description, FfxApiResourceState state);
FfxApiResource fsr3_vkpt_wrap_buffer(VkBuffer buffer, const FfxApiResourceDescription* description, FfxApiResourceState state);

#ifdef __cplusplus
}
#endif
