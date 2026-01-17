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

# Exclude legacy directory (standalone GLFW rendering - deprecated)
list(FILTER RENDERER_SOURCES EXCLUDE REGEX "/legacy/")

# Create the renderer library
add_library(ohao_renderer ${RENDERER_SOURCES})

target_include_directories(ohao_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
)

target_link_libraries(ohao_renderer PUBLIC
    ${Vulkan_LIBRARIES}
    ohao_engine
    ohao_utils
    glm
    VulkanMemoryAllocator
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDERER_SOURCES})
