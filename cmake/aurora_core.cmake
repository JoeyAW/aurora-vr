add_library(aurora_core STATIC
        lib/aurora.cpp
        lib/device.cpp
        lib/device.hpp
        lib/input.cpp
        lib/io.cpp
        lib/io.hpp
        lib/logging.cpp
        lib/system_info.cpp
        lib/system_info.hpp
        lib/thread.cpp
        lib/thread.hpp
        lib/time.cpp
        lib/time_internal.hpp
        lib/window.cpp
)
add_library(aurora::core ALIAS aurora_core)
set_target_properties(aurora_core PROPERTIES FOLDER "aurora")

target_compile_definitions(aurora_core PUBLIC AURORA TARGET_PC)
# CORRECTED 2026-09-16: briefly gated TARGET_PC behind `if (NOT ANDROID)`
# here, on the (wrong) assumption it meant "Windows/PC specifically". It
# doesn't -- per dolphin/types.h, TARGET_PC means "modern host platform
# using stdint.h/stdbool.h types" as opposed to the ORIGINAL GameCube/Wii
# Metrowerks target these dolphin/ headers were ported from -- BOOL, s8/
# u32/etc. all need it on Android too, and un-defining it broke those
# (unknown type name 'BOOL', etc.). The actual Windows-only code (VR debug
# logging via OutputDebugStringA/_snprintf_s in gx.cpp/GXLighting.cpp) is
# narrowed at its own #ifdef sites instead -- see those files.
target_include_directories(aurora_core PUBLIC include)
target_link_libraries(aurora_core PUBLIC fmt::fmt ${AURORA_SDL3_TARGET} xxHash::xxhash)
target_link_libraries(aurora_core PRIVATE absl::btree absl::flat_hash_map sqlite3 Tracy::TracyClient)
if (AURORA_ENABLE_GX AND AURORA_CACHE_USE_ZSTD)
    target_compile_definitions(aurora_core PRIVATE AURORA_CACHE_USE_ZSTD)
    target_link_libraries(aurora_core PRIVATE zstd::libzstd)
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL Windows)
    # stuff for fetching system info.
    target_link_libraries(aurora_core PRIVATE wbemuuid.lib comsuppw.lib ntdll.lib DXGI.lib)
elseif (APPLE)
    target_sources(aurora_core PRIVATE lib/system_info_mac.mm)
endif ()

if (IOS)
    find_library(COREHAPTICS_FRAMEWORK CoreHaptics REQUIRED)
    target_sources(aurora_core PRIVATE lib/device_ios.mm)
    set_source_files_properties(lib/device_ios.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
    target_link_libraries(aurora_core PUBLIC ${COREHAPTICS_FRAMEWORK})
endif ()

if (AURORA_ENABLE_GX)
    target_sources(aurora_core PRIVATE lib/imgui.cpp)
    target_link_libraries(aurora_core PUBLIC imgui)
endif ()

if(AURORA_ENABLE_RMLUI)
    target_compile_definitions(aurora_core PUBLIC AURORA_ENABLE_RMLUI)

    target_sources(aurora_core PRIVATE
            lib/rmlui.cpp
            lib/rmlui/RuntimeTextureProvider.cpp
            lib/rmlui/RmlUi_Backend_Aurora.cpp
            lib/rmlui/WebGPURenderInterface.cpp
            lib/rmlui/SystemInterface_Aurora.cpp
            lib/rmlui/FileInterface_SDL.cpp
            lib/rmlui/GlassFilter.cpp
            lib/rmlui/ImageEffects.cpp
    )
    target_link_libraries(aurora_core PUBLIC rmlui)

    target_link_libraries(aurora_core PUBLIC rmlui_backends)
endif ()

if (AURORA_ENABLE_GX)
    target_compile_definitions(aurora_core PUBLIC AURORA_ENABLE_GX WEBGPU_DAWN)
    target_sources(aurora_core PRIVATE
            lib/webgpu/gpu.cpp
            lib/webgpu/gpu_cache.cpp
            lib/webgpu/gpu_prof.cpp
            lib/dawn/BackendBinding.cpp
            lib/dawn/TracyPlatform.cpp
    )
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
        set_source_files_properties(lib/dawn/TracyPlatform.cpp PROPERTIES COMPILE_FLAGS -fno-rtti)
    endif ()
    target_link_libraries(aurora_core PRIVATE dawn::webgpu_dawn)
    if (DAWN_ENABLE_VULKAN)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_VULKAN)
    endif ()
    if (DAWN_ENABLE_METAL)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_METAL)
        target_sources(aurora_core PRIVATE lib/dawn/MetalBinding.mm)
        set_source_files_properties(lib/dawn/MetalBinding.mm PROPERTIES COMPILE_FLAGS -fobjc-arc)
        target_link_options(aurora_core PUBLIC "LINKER:-weak_framework,Metal")
    endif ()
    if (DAWN_ENABLE_D3D11)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_D3D11)
    endif ()
    if (DAWN_ENABLE_D3D12)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_D3D12)
    endif ()
    if (DAWN_ENABLE_DESKTOP_GL OR DAWN_ENABLE_OPENGLES)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_OPENGL)
        if (DAWN_ENABLE_DESKTOP_GL)
            target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_DESKTOP_GL)
        endif ()
        if (DAWN_ENABLE_OPENGLES)
            target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_OPENGLES)
        endif ()
    endif ()
    if (DAWN_ENABLE_NULL)
        target_compile_definitions(aurora_core PRIVATE DAWN_ENABLE_BACKEND_NULL)
    endif ()
endif ()
