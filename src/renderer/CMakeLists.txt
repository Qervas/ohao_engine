# OHAO Renderer Library
# ======================
#
# Architecture:
# - offscreen/     - OffscreenRenderer for Godot GDExtension (headless Vulkan)
# - camera/        - Camera utilities (used by both offscreen and legacy)
# - components/    - MeshComponent, MaterialComponent (data-only, no Vulkan handles)
# - material/      - Material struct (PBR properties, no Vulkan dependencies)
#
# Legacy (not built):
# - legacy/        - VulkanContext + GLFW standalone rendering (deprecated)
#                   Contains: VulkanContext, Surface, SwapChain, RHI wrappers,
#                   shader management, texture loading, scene rendering subsystems
#
# The legacy directory contains the full standalone GLFW-based renderer that
# was used before Godot integration. It's preserved for reference but not compiled.

# Find all source files
file(GLOB_RECURSE RENDERER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

# Exclude legacy and disabled directories
list(FILTER RENDERER_SOURCES EXCLUDE REGEX "/legacy/")
list(FILTER RENDERER_SOURCES EXCLUDE REGEX "/_disabled/")

# Create the renderer library
add_library(ohao_renderer ${RENDERER_SOURCES})

# OptiX SDK (header-only, runtime-loaded)
set(OPTIX_PATH "C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.1.0" CACHE PATH "OptiX SDK path")
find_package(CUDAToolkit QUIET)

target_include_directories(ohao_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
    "${OPTIX_PATH}/include"
)

if(CUDAToolkit_FOUND)
    target_include_directories(ohao_renderer PUBLIC ${CUDAToolkit_INCLUDE_DIRS})
endif()

target_link_libraries(ohao_renderer PUBLIC
    ${Vulkan_LIBRARIES}
    ohao_engine
    ohao_animation
    ohao_utils
    glm
    VulkanMemoryAllocator
)

if(CUDAToolkit_FOUND)
    target_link_libraries(ohao_renderer PUBLIC CUDA::cudart CUDA::cuda_driver)
    message(STATUS "CUDA found: ${CUDAToolkit_VERSION} — OptiX denoiser ENABLED")
else()
    message(STATUS "CUDA not found — OptiX denoiser disabled")
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDERER_SOURCES})
