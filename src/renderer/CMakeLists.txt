# Find all source files in vk directory
file(GLOB_RECURSE RENDERER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

# Create the renderer library with all sources
add_library(ohao_renderer
    vulkan_context.cpp
    ${RENDERER_SOURCES}
)

target_include_directories(ohao_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external/imgui
    ${CMAKE_SOURCE_DIR}/external/imgui/backends
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_renderer PUBLIC
    ${Vulkan_LIBRARIES}
    glfw
    ohao_core
    imgui
    imgui_stdlib

)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDERER_SOURCES})
