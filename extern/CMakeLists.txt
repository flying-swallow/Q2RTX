
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
    option(ENABLE_HLSL OFF)
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

#
# LiteRT (experimental AI upscaler backend)
#

if(USE_LITE_RT)
    set(LITERT_ENABLE_GPU OFF CACHE BOOL "" FORCE)
    set(LITERT_ENABLE_NPU ON CACHE BOOL "" FORCE)
    # Qualcomm QNN dispatch backend reaches the Hexagon NPU (HTP). Its headers
    # (QAIRT_HEADERS_DIR) are auto-fetched by litert/vendors/CMakeLists.txt
    # from Qualcomm's public "Community" QAIRT SDK download if not already
    # set, so no manual SDK install is required to configure the build.
    # The dispatch backend itself builds as a standalone plugin
    # (libLiteRtDispatch_Qualcomm.so, litert/vendors/qualcomm/dispatch/) that
    # LiteRT dlopen()s at runtime via kLiteRtEnvOptionTagDispatchLibraryDir --
    # it is not linked into the `litert` interface target below. It must be
    # deployed next to the game binary and the QNN runtime .so's for the
    # target platform (aarch64-oe-linux-gcc11.2) staged from the same fetched
    # QAIRT SDK tree (see deploy-assets.sh).
    set(LITERT_ENABLE_QUALCOMM ON CACHE BOOL "" FORCE)
    set(LITERT_ENABLE_SAMSUNG OFF CACHE BOOL "" FORCE)
    set(TFLITE_ENABLE_GPU OFF CACHE BOOL "" FORCE)
    set(LITERT_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    # litert_c_api's INTERFACE_INCLUDE_DIRECTORIES and litert_runtime's
    # open_cl_memory.cc/open_cl_sync.cc (unconditionally compiled, gated only
    # at the class-body level by LITERT_HAS_OPENCL_SUPPORT) both need real
    # <CL/cl.h> headers at ${CMAKE_BINARY_DIR}/opencl_headers even with GPU
    # disabled, since tflite's own opencl_headers fetch is skipped when
    # TFLITE_ENABLE_GPU=OFF. Fetch the same header-only repo/tag tflite uses.
    include(FetchContent)
    FetchContent_Declare(
        opencl_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenCL-Headers
        GIT_TAG dcd5bede6859d26833cd85f0d6bbcee7382dc9b3
        SOURCE_DIR "${CMAKE_BINARY_DIR}/opencl_headers"
    )
    FetchContent_Populate(opencl_headers)

    add_subdirectory(LiteRT/litert)

    # litert_c_api, litert_runtime, litert_core (and their satellite static
    # libs) reference symbols back and forth across archive boundaries
    # (e.g. litert_runtime calls into litert_c_api's option accessors, while
    # litert_core tears down objects defined in litert_runtime). Plain
    # left-to-right archive linking drops unresolved refs once an archive is
    # passed, so wrap the whole closure in a RESCAN group to make the linker
    # keep re-scanning until everything resolves.
    add_library(litert INTERFACE IMPORTED GLOBAL)
    target_link_libraries(litert INTERFACE
        "$<LINK_GROUP:RESCAN,litert_c_api,litert_runtime,litert_core,litert_cc_api,litert_cc_internal,litert_core_cache,litert_core_model,litert_compiler_plugin,litert_logging,litert_c_options,litert_cc_options,tensorflow-lite>"
    )
endif()
