# NRD — NVIDIA RayTracingDenoiser (MIT License)
# Spatio-temporal denoiser library used for realtime RT diffuse/specular/shadow
# signals. Integrated as a hard CMake dependency via FetchContent (Sub-plan 4.A).
# https://github.com/NVIDIAGameWorks/RayTracingDenoiser

include(FetchContent)

# Trim NRD's build surface. NRD's CMake exposes several toggles that default
# ON and pull in tools we don't need (sample, integration helpers, CI shader
# validation). Keep only the core library + SPIR-V shaders for Vulkan.
# These are set as CACHE BOOL before MakeAvailable so NRD picks them up.
set(NRD_DISABLE_SHADER_COMPILATION OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS        OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXIL_SHADERS        OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS       ON  CACHE BOOL "" FORCE)
set(NRD_STATIC_LIBRARY             ON  CACHE BOOL "" FORCE)

FetchContent_Declare(
    NRD
    GIT_REPOSITORY https://github.com/NVIDIAGameWorks/RayTracingDenoiser.git
    GIT_TAG        v4.17.2
)
FetchContent_MakeAvailable(NRD)

if(TARGET NRD)
    message(STATUS "NRD: target 'NRD' available (pinned tag v4.17.2)")
else()
    message(WARNING "NRD: target 'NRD' NOT found after FetchContent_MakeAvailable")
endif()
