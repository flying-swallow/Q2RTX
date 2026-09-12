set(SHADER_SOURCE_DEPENDENCIES
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/nrd_common.glsl
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/asvgf.glsl
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/brdf.glsl
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/constants.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/global_textures.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/global_ubo.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/god_rays_shared.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/light_lists.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/path_tracer_rgen.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/path_tracer.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/path_tracer_hit_shaders.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/path_tracer_transparency.glsl
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/precomputed_sky.glsl
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/precomputed_sky_params.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/projection.glsl
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/sky.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/tiny_encryption_algorithm.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/tone_mapping_utils.glsl
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/utils.glsl
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/vertex_buffer.h
    ${CMAKE_SOURCE_DIR}/src/refresh/vkpt/shader/water.glsl)

if(TARGET glslang-standalone)
    set(GLSLANG_COMPILER "$<TARGET_FILE:glslang-standalone>")
    message(STATUS "Using glslang built from source")
else()
    find_program(GLSLANG_COMPILER NAMES glslang glslangValidator PATHS "$ENV{VULKAN_SDK}/bin/")

    if(NOT GLSLANG_COMPILER)
        message(FATAL_ERROR "Couldn't find glslang! "
            "Please provide a valid path to it using the GLSLANG_COMPILER variable.")
    endif()
    
    message(STATUS "Using this glslang: ${GLSLANG_COMPILER}")
endif()

# spirv-opt, used to shrink the ray-query compute variants of the path tracer
# shaders (see the OPTIMIZE option of compile_shader below). The bundled
# glslang is built with ENABLE_OPT=OFF -- it advertises -Os but errors out with
# "optimizer not linked" -- so we shell out to a standalone spirv-opt instead.
OPTION(USE_SPIRV_OPT "Run spirv-opt -Os on shaders that request it (needed for ray-query on Adreno)" ON)

if(USE_SPIRV_OPT)
    # VULKAN_SDK isn't always exported, so also probe the default install root.
    file(GLOB VULKAN_SDK_BIN_DIRS "C:/VulkanSDK/*/Bin")
    find_program(SPIRV_OPT_COMMAND NAMES spirv-opt
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" ${VULKAN_SDK_BIN_DIRS})

    if(SPIRV_OPT_COMMAND)
        message(STATUS "Using this spirv-opt: ${SPIRV_OPT_COMMAND}")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        # Adreno's compute-shader compiler rejects the unoptimized path tracer
        # shaders outright (vkCreateComputePipelines -> VK_ERROR_UNKNOWN), so on
        # ARM64 this isn't merely an optimization -- the build won't run.
        message(WARNING "spirv-opt not found. On Adreno/ARM64 the unoptimized ray-query "
            "path tracer shaders exceed the driver's shader compiler limits and the game "
            "will fail with 'Couldn't initialize pt'. Install the Vulkan SDK or set "
            "SPIRV_OPT_COMMAND.")
    else()
        message(STATUS "spirv-opt not found, shaders will not be size-optimized.")
    endif()
endif()

# Collect additional glslangValidator args
set(GLSLANG_ARGS)
if(CONFIG_BUILD_SHADER_DEBUG_INFO)
    list(APPEND GLSLANG_ARGS -gVS)
endif()

# Write args to a file. Used to trigger rebuild if they change
set(COMPILE_ARGS_DEP "${CMAKE_BINARY_DIR}/compile_shader.dep")
file(CONFIGURE OUTPUT "${COMPILE_ARGS_DEP}" CONTENT "@GLSLANG_ARGS@:@SPIRV_OPT_COMMAND@")

function(compile_shader)
    set(options OPTIMIZE)
    set(oneValueArgs SOURCE_FILE OUTPUT_FILE_NAME OUTPUT_FILE_LIST STAGE)
    set(multiValueArgs DEFINES INCLUDES)
    cmake_parse_arguments(params "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT params_SOURCE_FILE)
        message(FATAL_ERROR "compile_shader: SOURCE_FILE argument missing")
    endif()

    if (NOT params_OUTPUT_FILE_LIST)
        message(FATAL_ERROR "compile_shader: OUTPUT_FILE_LIST argument missing")
    endif()

    set(src_file "${CMAKE_CURRENT_SOURCE_DIR}/${params_SOURCE_FILE}")

    if (params_OUTPUT_FILE_NAME)
        set(output_file_name ${params_OUTPUT_FILE_NAME})
    else()
        get_filename_component(output_file_name ${src_file} NAME)
    endif()

    if (params_STAGE)
        set(stage -S comp)
    else()
        set(stage)
    endif()
    
    set_source_files_properties(${src_file} PROPERTIES VS_TOOL_OVERRIDE "None")

    set (out_dir "${CMAKE_SOURCE_DIR}/baseq2/shader_vkpt")
    set (out_file "${out_dir}/${output_file_name}.spv")
    
    set(glslang_command_line
            ${stage}
            --target-env vulkan1.2
            --quiet
            -DVKPT_SHADER
            -V
            ${GLSLANG_ARGS}
            ${params_DEFINES}
            ${params_INCLUDES}
            "${src_file}"
            -o "${out_file}")

    # Optional in-place size optimization. Shrinks the path tracer's ray-query
    # compute variants by 35-55%, which is what keeps them under the Adreno
    # shader compiler's (undocumented) size limit -- without it those pipelines
    # fail to create with VK_ERROR_UNKNOWN. Harmless elsewhere.
    set(optimize_command)
    if (params_OPTIMIZE AND SPIRV_OPT_COMMAND)
        set(optimize_command COMMAND ${SPIRV_OPT_COMMAND} -Os "${out_file}" -o "${out_file}")
    endif()

    add_custom_command(OUTPUT ${out_file}
                       DEPENDS ${src_file}
                       DEPENDS ${SHADER_SOURCE_DEPENDENCIES}
                       DEPENDS ${COMPILE_ARGS_DEP}
                       MAIN_DEPENDENCY ${src_file}
                       COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
                       COMMAND ${GLSLANG_COMPILER} ${glslang_command_line}
                       ${optimize_command})
    
    set(${params_OUTPUT_FILE_LIST} ${${params_OUTPUT_FILE_LIST}} ${out_file} PARENT_SCOPE)
endfunction()

# FSR3 is shipped as HLSL in the FidelityFX SDK.  Keep this path separate from
# compile_shader(): the latter intentionally retains the project's GLSL
# invocation and dependency behavior.
include(${CMAKE_SOURCE_DIR}/src/refresh/vkpt/fsr3/fsr3_shader_manifest.cmake)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "FSR3 shader builds require a Python 3 interpreter")
endif()

if(CONFIG_BUILD_GLSLANG AND TARGET glslang-standalone)
    set(FSR3_GLSLANG_COMPILER "$<TARGET_FILE:glslang-standalone>")
    message(STATUS "Using bundled glslang for FSR3 HLSL shaders")
else()
    find_program(FSR3_GLSLANG_COMPILER NAMES glslangValidator glslang
        PATHS "$ENV{VULKAN_SDK}/bin/")
    if(NOT FSR3_GLSLANG_COMPILER)
        message(FATAL_ERROR "FSR3 shader builds require glslangValidator with HLSL support")
    endif()
    message(STATUS "Using system glslang for FSR3 HLSL shaders: ${FSR3_GLSLANG_COMPILER}")
endif()

set(FSR3_SHADER_HELPER
    "${CMAKE_SOURCE_DIR}/src/refresh/vkpt/fsr3/compile_fsr3_shader.py")
set(FSR3_SHADER_DEPENDENCIES
    ${Q2RTX_FSR3_SHADER_MANIFEST}
    ${FSR3_SHADER_HELPER}
    ${Q2RTX_FSR3_SHADER_SOURCES})

function(compile_fsr3_shader)
    set(options)
    set(oneValueArgs NAME SOURCE SHARPEN OUTPUT_FILE_LIST)
    cmake_parse_arguments(params "${options}" "${oneValueArgs}" "" ${ARGN})

    if(NOT params_NAME OR NOT params_SOURCE OR NOT params_OUTPUT_FILE_LIST)
        message(FATAL_ERROR "compile_fsr3_shader requires NAME, SOURCE, and OUTPUT_FILE_LIST")
    endif()

    set(src_file "${CMAKE_SOURCE_DIR}/${params_SOURCE}")
    set(out_dir "${CMAKE_SOURCE_DIR}/baseq2/shader_vkpt/fsr3")
    set(out_file "${out_dir}/${params_NAME}.spv")
    set(metadata_file "${out_dir}/${params_NAME}.json")
    set(sharpen_define 0)
    if(params_SHARPEN)
        set(sharpen_define 1)
    endif()

    set(defines)
    foreach(define ${Q2RTX_FSR3_SHADER_DEFINES})
        list(APPEND defines --define ${define})
    endforeach()
    set(includes)
    foreach(include_dir ${Q2RTX_FSR3_SHADER_INCLUDE_DIRS})
        list(APPEND includes --include ${include_dir})
    endforeach()

    add_custom_command(OUTPUT ${out_file} ${metadata_file}
        DEPENDS ${src_file} ${FSR3_SHADER_DEPENDENCIES}
        MAIN_DEPENDENCY ${src_file}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${Python3_EXECUTABLE} ${FSR3_SHADER_HELPER}
            --compiler ${FSR3_GLSLANG_COMPILER}
            --source ${src_file}
            --output ${out_file}
            --metadata ${metadata_file}
            ${includes}
            ${defines}
            --define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=${sharpen_define}
        VERBATIM)

    set(${params_OUTPUT_FILE_LIST} ${${params_OUTPUT_FILE_LIST}}
        ${out_file} ${metadata_file} PARENT_SCOPE)
endfunction()

set(Q2RTX_FSR3_SHADER_BYTECODE)
set(Q2RTX_FSR3_SHADER_MANIFEST_ROWS)
foreach(pass ${Q2RTX_FSR3_SHADER_PASSES})
    string(REPLACE "|" ";" pass_fields "${pass}")
    list(GET pass_fields 0 pass_name)
    list(GET pass_fields 1 pass_source)
    list(GET pass_fields 2 pass_sharpen)
    list(GET pass_fields 3 pass_id)
    # SDK permutation flags: HDR=2, render-resolution motion vectors=4,
    # sharpening=32.  The Lanczos LUT bit (1) is not set because the shaders
    # are compiled with the reference Lanczos path (REPROJECT_USE_LANCZOS_TYPE
    # left at its default of 0), which is what the SDK requests unless the
    # backend reports a 32/64-lane device.
    math(EXPR pass_options "6 + (${pass_sharpen} * 32)")
    compile_fsr3_shader(NAME ${pass_name} SOURCE ${pass_source}
        SHARPEN ${pass_sharpen} OUTPUT_FILE_LIST Q2RTX_FSR3_SHADER_BYTECODE)
    list(APPEND Q2RTX_FSR3_SHADER_MANIFEST_ROWS
        "${pass_name}|${pass_id}|${pass_options}|${CMAKE_SOURCE_DIR}/baseq2/shader_vkpt/fsr3/${pass_name}.spv|${CMAKE_SOURCE_DIR}/baseq2/shader_vkpt/fsr3/${pass_name}.json")
endforeach()
list(APPEND shader_bytecode ${Q2RTX_FSR3_SHADER_BYTECODE})
