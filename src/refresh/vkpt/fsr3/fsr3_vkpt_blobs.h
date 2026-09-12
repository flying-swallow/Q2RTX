#ifndef Q2RTX_FSR3_VKPT_BLOBS_H
#define Q2RTX_FSR3_VKPT_BLOBS_H

#include <stdint.h>

#if !defined(_MSC_VER) && !defined(__declspec)
#define __declspec(x)
#define Q2RTX_FSR3_UNDEF_DECLSPEC
#endif
#include "../../../../extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/include/ffx_fsr3upscaler.h"
#ifdef Q2RTX_FSR3_UNDEF_DECLSPEC
#undef __declspec
#undef Q2RTX_FSR3_UNDEF_DECLSPEC
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Vulkan equivalent of the SDK's internal permutation lookup. */
FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(
    FfxFsr3UpscalerPass passId,
    uint32_t permutationOptions,
    FfxShaderBlob* outBlob);

#ifdef __cplusplus
}
#endif

#endif
