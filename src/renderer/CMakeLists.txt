add_library(ohao_renderer
    vulkan_context.cpp
)

target_include_directories(ohao_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_renderer PUBLIC
    ${Vulkan_LIBRARIES}
    glfw
)
