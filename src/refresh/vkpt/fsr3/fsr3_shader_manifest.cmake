# The live FSR3 upscaler passes from ffx_fsr3upscaler.cpp.  TCR_AUTOGENERATE
# is deprecated by the SDK and deliberately is not part of this manifest.
set(Q2RTX_FSR3_SHADER_MANIFEST "${CMAKE_CURRENT_LIST_FILE}")
set(Q2RTX_FSR3_SHADER_PASSES
    "fsr3_prepare_inputs|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_prepare_inputs_pass.hlsl|0|0"
    "fsr3_luma_pyramid|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_luma_pyramid_pass.hlsl|0|1"
    "fsr3_shading_change_pyramid|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_shading_change_pyramid_pass.hlsl|0|2"
    "fsr3_shading_change|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_shading_change_pass.hlsl|0|3"
    "fsr3_prepare_reactivity|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_prepare_reactivity_pass.hlsl|0|4"
    "fsr3_luma_instability|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_luma_instability_pass.hlsl|0|5"
    "fsr3_accumulate|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_accumulate_pass.hlsl|0|6"
    "fsr3_accumulate_sharpen|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_accumulate_pass.hlsl|1|7"
    "fsr3_rcas|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_rcas_pass.hlsl|0|8"
    "fsr3_debug_view|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_debug_view_pass.hlsl|0|9"
    "fsr3_generate_reactive|extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_autogen_reactive_pass.hlsl|0|10")

set(Q2RTX_FSR3_SHADER_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX/api/internal/gpu"
    "${CMAKE_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/include/gpu")

# Fixed Q2RTX permutation: HDR/linear input, render-resolution motion vectors,
# unjittered vectors, normal finite depth, FP32,
# wave32.  Sharpening is the sole per-pass override in the manifest above.
set(Q2RTX_FSR3_SHADER_DEFINES
    FFX_HLSL=1
    FFX_GPU=1
    FFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0
    FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=1
    FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1
    FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0
    FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0
    FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
    FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
    FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
    FFX_HALF=0
    FFX_HLSL_SM=60)

file(GLOB_RECURSE Q2RTX_FSR3_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX/api/internal/gpu/*.h"
    "${CMAKE_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/include/gpu/*.h"
    "${CMAKE_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/fsr3/internal/shaders/*.hlsl")
