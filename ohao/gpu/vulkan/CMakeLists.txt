file(GLOB_RECURSE GPU_VK_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
file(GLOB_RECURSE GPU_VK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(ohao_gpu_vulkan STATIC ${GPU_VK_HEADERS} ${GPU_VK_SOURCES})

target_include_directories(ohao_gpu_vulkan PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
)

target_link_libraries(ohao_gpu_vulkan PUBLIC
    ohao_scene
    ohao_core
    glm
    VulkanMemoryAllocator
    ${Vulkan_LIBRARIES}
)
