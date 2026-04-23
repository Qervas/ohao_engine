# OHAO Render Library — rendering algorithms, passes, RT
# GPU abstraction lives in gpu/vulkan/

find_package(OpenImageDenoise REQUIRED)

# --- OptiX SDK detection (Denoiser Sub-plan 2) ---
# Header-only SDK. Headers live under $OPTIX_ROOT/include/. CUDA runtime
# is linked separately. If either is missing, OHAO_HAS_OPTIX stays undef
# and optix_denoise.cpp compiles a no-op stub (runtime falls back to OIDN).

find_path(OPTIX_INCLUDE_DIR optix.h
    HINTS
        $ENV{OPTIX_ROOT}
        $ENV{HOME}/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
        /opt/optix-sdk
        /usr/local/optix
    PATH_SUFFIXES include
)
find_package(CUDAToolkit QUIET)

if(OPTIX_INCLUDE_DIR AND CUDAToolkit_FOUND)
    message(STATUS "OptiX SDK found: ${OPTIX_INCLUDE_DIR}")
    message(STATUS "  CUDA Toolkit: ${CUDAToolkit_VERSION}")
    set(OHAO_OPTIX_AVAILABLE TRUE)
else()
    set(OHAO_OPTIX_AVAILABLE FALSE)
    if(NOT OPTIX_INCLUDE_DIR)
        message(STATUS "OptiX SDK: not found (set OPTIX_ROOT env var) — OptiX denoiser unavailable")
    endif()
    if(NOT CUDAToolkit_FOUND)
        message(STATUS "CUDA Toolkit: not found — OptiX denoiser unavailable")
    endif()
endif()

file(GLOB_RECURSE RENDERER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

# Exclude disabled passes
list(FILTER RENDERER_SOURCES EXCLUDE REGEX "/_disabled/")

add_library(ohao_renderer ${RENDERER_SOURCES})

target_include_directories(ohao_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
)

target_link_libraries(ohao_renderer PUBLIC
    ohao_gpu_vulkan
    ohao_scene
    ohao_animation
    glm
    VulkanMemoryAllocator
    ${Vulkan_LIBRARIES}
    OpenImageDenoise
)

# Conditional OptiX support
if(OHAO_OPTIX_AVAILABLE)
    target_compile_definitions(ohao_renderer PUBLIC OHAO_HAS_OPTIX)
    target_include_directories(ohao_renderer PRIVATE ${OPTIX_INCLUDE_DIR})
    target_link_libraries(ohao_renderer PUBLIC CUDA::cudart CUDA::cuda_driver)
endif()

# Conditional NRD support (Sub-plan 4.A). nrd_denoise.cpp is always in
# RENDERER_SOURCES via the glob; its body is guarded by OHAO_NRD_ENABLED
# so it compiles to an empty TU when OHAO_NRD=OFF.
if(OHAO_NRD)
    target_compile_definitions(ohao_renderer PRIVATE OHAO_NRD_ENABLED)
    target_link_libraries(ohao_renderer PRIVATE NRD)
    # Sub-plan 4.C T3a: our own NRDIntegration static lib is named
    # OhaoNRDIntegration to avoid clash with NRD's upstream INTERFACE target
    # (which points at nrd-src/Integration/ and carries no symbols).
    if(TARGET OhaoNRDIntegration)
        target_link_libraries(ohao_renderer PRIVATE OhaoNRDIntegration)
        target_compile_definitions(ohao_renderer PRIVATE OHAO_NRD_INTEGRATION_AVAILABLE)
    endif()
endif()
