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

# Pin NRD normal+roughness encoding to R10G10B10A2_UNORM (rotated oct + sign-in-roughness
# + 2-bit materialID). This is NRD's upstream default, but making it explicit protects
# us from future NRD bumps silently flipping the encoding.
# Roughness encoding: LINEAR (1) = raw linear roughness stored as-is (NRD default).
set(NRD_NORMAL_ENCODING    "2" CACHE STRING "" FORCE)
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "" FORCE)

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

# Sub-plan 4.C: NRDIntegration — NVIDIA's reference Vulkan integration helper,
# vendored into external/nrd_integration/. Wraps NRD's "recipe" API
# (GetComputeDispatches, GetInstanceDesc) with concrete Vulkan pipeline,
# descriptor pool, texture pool, and barrier management.
set(OHAO_NRD_INTEGRATION_DIR ${CMAKE_SOURCE_DIR}/external/nrd_integration)

file(GLOB OHAO_NRD_INTEGRATION_SOURCES
    "${OHAO_NRD_INTEGRATION_DIR}/*.cpp"
    "${OHAO_NRD_INTEGRATION_DIR}/*.h"
    "${OHAO_NRD_INTEGRATION_DIR}/*.hpp"
)

if(OHAO_NRD_INTEGRATION_SOURCES)
    set(OHAO_NRD_HAS_CPP OFF)
    foreach(src ${OHAO_NRD_INTEGRATION_SOURCES})
        if(src MATCHES "\\.cpp$")
            set(OHAO_NRD_HAS_CPP ON)
        endif()
    endforeach()
    if(OHAO_NRD_HAS_CPP)
        add_library(NRDIntegration STATIC ${OHAO_NRD_INTEGRATION_SOURCES})
        target_include_directories(NRDIntegration PUBLIC
            ${OHAO_NRD_INTEGRATION_DIR}
            ${Vulkan_INCLUDE_DIRS}
        )
        target_link_libraries(NRDIntegration PUBLIC NRD ${Vulkan_LIBRARIES})
        target_compile_features(NRDIntegration PUBLIC cxx_std_17)
    else()
        message(STATUS "NRD: NRDIntegration vendored as headers-only (v4.17.2 .hpp-impl depends on NRI); skipping static lib")
    endif()
endif()
