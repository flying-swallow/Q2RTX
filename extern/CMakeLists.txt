
#
# ZLIB
#

if(USE_SYSTEM_ZLIB)
    find_package(ZLIB REQUIRED)

    add_library(z INTERFACE IMPORTED GLOBAL)
    target_link_libraries(z INTERFACE ZLIB::ZLIB)
else()
    add_subdirectory(zlib)
    target_include_directories(zlibstatic PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/zlib> $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/zlib>)

    set_target_properties(zlib PROPERTIES FOLDER extern)
    set_target_properties(zlibstatic PROPERTIES FOLDER extern POSITION_INDEPENDENT_CODE ON)
    set_target_properties(minigzip PROPERTIES FOLDER extern)
    set_target_properties(example PROPERTIES FOLDER extern)

    add_library(z INTERFACE IMPORTED GLOBAL)
    target_link_libraries(z INTERFACE zlibstatic)
    target_include_directories(z INTERFACE "${ZLIB_INCLUDE_DIRS}")
endif()

#
# glslangValidator
#

if(CONFIG_BUILD_GLSLANG)
    option(BUILD_EXTERNAL OFF)
    option(ENABLE_SPVREMAPPER OFF)
    # FSR3 shader compilation uses the bundled glslang HLSL front end.  FORCE
    # is needed because a prior configure may have cached the standalone
    # glslang default (ENABLE_HLSL=OFF).
    set(ENABLE_HLSL ON CACHE BOOL "Enables HLSL input support" FORCE)
    option(ENABLE_OPT OFF)
    option(ENABLE_PCH OFF)
    option(ENABLE_CTEST OFF)

    add_subdirectory(glslang)

    set_target_properties(glslang-standalone PROPERTIES FOLDER glslang)
endif()

#
# SDL 2
#

# On a 32-bit target, only the server is built, and it doesn't need SDL
if (IS_64_BIT)
  if (USE_SYSTEM_SDL2)
      find_package(SDL2 REQUIRED)

      add_library(SDL2 INTERFACE IMPORTED GLOBAL)
      target_link_libraries(SDL2 INTERFACE SDL2::SDL2)
    else()
      option(SDL_SHARED "" OFF)
      option(SDL_STATIC "" ON)
      option(SDL_SHARED_ENABLED_BY_DEFAULT OFF)
      option(SDL_TEST OFF)

      if (CONFIG_LINUX_STEAM_RUNTIME_SUPPORT)
          option(INPUT_TSLIB OFF)
      endif()

      add_subdirectory(SDL2)

      set_target_properties(SDL2main PROPERTIES FOLDER extern)
      set_target_properties(SDL2-static PROPERTIES FOLDER extern)
      set_target_properties(uninstall PROPERTIES FOLDER extern)
      set_target_properties(sdl_headers_copy PROPERTIES FOLDER extern)

      add_library(SDL2 INTERFACE IMPORTED GLOBAL)
      target_link_libraries(SDL2 INTERFACE SDL2main SDL2-static)
    endif()
endif()

#
# CURL
#
if(CONFIG_USE_CURL)
    if(USE_SYSTEM_CURL)
        find_package(CURL REQUIRED)
        add_library(libcurl INTERFACE IMPORTED GLOBAL)
        target_link_libraries(libcurl INTERFACE CURL::libcurl)
    else()
        option(BUILD_CURL_EXE "" OFF)
        option(BUILD_SHARED_LIBS "" OFF)
        option(CURL_STATICLIB "" ON)
        option(BUILD_TESTING "" OFF)
        option(ENABLE_MANUAL "" OFF)
        option(ENABLE_DOCS "" OFF)
        option(HTTP_ONLY "" ON)
        option(CURL_ZLIB "" OFF)
        option(CMAKE_USE_OPENSSL "" OFF)
        set(CURL_CA_PATH "none" CACHE PATH "")

        set(CMAKE_DEBUG_POSTFIX "" CACHE STRING "Override CMAKE_DEBUG_POSTFIX, which curl sets to '-d'")
    
        set(ZLIB_DIR "{CMAKE_CURRENT_SOURCE_DIR}/zlib")

        add_subdirectory(curl)

        set_target_properties(libcurl_static PROPERTIES FOLDER extern)
        if (TARGET libcurl_object)
            set_target_properties(libcurl_object PROPERTIES FOLDER extern)
        endif()
        set_target_properties(curl_uninstall PROPERTIES FOLDER extern)
    endif()
endif()


add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)

add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tinyobjloader-c)

# FSR 3.1 host implementation. The Vulkan renderer owns the backend, so this
# target contains only the SDK's renderer-independent upscaler and the small
# set of API support files it directly needs. In particular, do not pull in
# frame generation, DX12 backends, or generated SDK shader blobs.
if (CONFIG_VKPT_RENDERER)
    set(FFX_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/FidelityFX-SDK/Kits/FidelityFX")
    add_library(ffx_fsr3upscaler STATIC
        "${FFX_SDK_ROOT}/upscalers/fsr3/internal/ffx_fsr3upscaler.cpp"
        "${FFX_SDK_ROOT}/api/internal/ffx_assert.cpp"
        "${FFX_SDK_ROOT}/api/internal/ffx_object_management.cpp"
    )
    target_include_directories(ffx_fsr3upscaler PRIVATE
        "${FFX_SDK_ROOT}/api/include"
        "${FFX_SDK_ROOT}/api/internal"
        "${FFX_SDK_ROOT}/upscalers/fsr3/include"
        "${CMAKE_SOURCE_DIR}/src/refresh/vkpt/fsr3"
    )
    # The staged SDK omits amdinternal/api/internal/git_hash_branch.h.  The
    # guarded include only supplies watermark diagnostics, which this host
    # target does not use; keep the workaround local to this target.
    target_compile_definitions(ffx_fsr3upscaler PRIVATE _GAMING_XBOX=1 FFX_GCC=1)
    # The SDK's opaque context default is too small for this SDK revision's
    # FSR3 private state. Consumers of the public header must use the same
    # value when this target is integrated.
    target_compile_definitions(ffx_fsr3upscaler PRIVATE FFX_SDK_DEFAULT_CONTEXT_SIZE=262144)
    if (NOT MSVC)
        target_compile_options(ffx_fsr3upscaler PRIVATE
            -include "${CMAKE_SOURCE_DIR}/src/refresh/vkpt/fsr3/fsr3_sdk_compat.h")
    endif()
    set_target_properties(ffx_fsr3upscaler PROPERTIES FOLDER extern)
endif()

# NRD is required by VKPT. Its FetchContent dependencies (ShaderMake, MathLib,
# and NRI) are intentionally kept out of GL-only builds.
if (CONFIG_VKPT_RENDERER)
    set(NRD_STATIC_LIBRARY ON CACHE BOOL "Build static library" FORCE)
    set(NRD_NRI ON CACHE BOOL "Pull, build and include NRI into NRD SDK package" FORCE)
    set(NRD_EMBEDS_SPIRV_SHADERS ON CACHE BOOL "NRD embeds SPIRV shaders" FORCE)
    set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "NRD embeds DXIL shaders" FORCE)
    set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "NRD embeds DXBC shaders" FORCE)
    set(NRD_SUPPORTS_QUAD_INTRINSICS OFF CACHE BOOL "Enable quad intrinsics" FORCE)
    set(NRD_NORMAL_ENCODING 4 CACHE STRING "Normal encoding variant" FORCE)
    set(NRD_ROUGHNESS_ENCODING 1 CACHE STRING "Roughness encoding variant" FORCE)

    add_subdirectory(NRD EXCLUDE_FROM_ALL)
endif()

#
# openal-soft (64-bit only, not needed for the 32-bit server)
#

if (IS_64_BIT)
    if (USE_SYSTEM_OPENAL)
        find_package(OpenAL REQUIRED)

        # Provide "OpenAL" target with result of CMake's detection
        add_library(OpenAL INTERFACE IMPORTED GLOBAL)
        target_link_libraries(OpenAL INTERFACE ${OPENAL_LIBRARY})
        target_include_directories(OpenAL INTERFACE ${OPENAL_INCLUDE_DIR})
    else()
        set(LIBTYPE STATIC)
        option(FORCE_STATIC_VCRT "" ON)
        option(ALSOFT_UTILS "" OFF)
        option(ALSOFT_EXAMPLES "" OFF)
        option(ALSOFT_UPDATE_BUILD_VERSION "" OFF)
        option(ALSOFT_INSTALL "" OFF)
        option(ALSOFT_INSTALL_CONFIG "" OFF)
        option(ALSOFT_INSTALL_HRTF_DATA "" OFF)
        option(ALSOFT_INSTALL_AMBDEC_PRESETS "" OFF)
        option(ALSOFT_INSTALL_EXAMPLES "" OFF)
        option(ALSOFT_INSTALL_UTILS "" OFF)
        option(ALSOFT_ENABLE_MODULES "" OFF)
        add_subdirectory(openal-soft)
        set_target_properties(OpenAL PROPERTIES FOLDER extern/openal-soft)
        set_target_properties(alsoft.common PROPERTIES FOLDER extern/openal-soft)
        set_target_properties(alsoft.excommon PROPERTIES FOLDER extern/openal-soft)
        set_target_properties(alsoft.fmt PROPERTIES FOLDER extern/openal-soft)
        set_target_properties(clang-tidy-check PROPERTIES FOLDER extern/openal-soft)
        target_compile_definitions(OpenAL PUBLIC AL_LIBTYPE_STATIC)
    endif()
endif()
