add_library(ohao_core
    window.cpp
)

target_include_directories(ohao_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_core PUBLIC
    ${Vulkan_LIBRARIES}
    glfw
)
