# Find all source files in vk directory
file(GLOB VK_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/vk/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/vk/*.hpp"
)

# Create the renderer library with all sources
add_library(ohao_renderer
    vulkan_context.cpp
    ${VK_SOURCES}
)

target_include_directories(ohao_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vk
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_renderer PUBLIC
    ${Vulkan_LIBRARIES}
    glfw
    ohao_core
)
