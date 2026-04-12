# OHAO Render Library — rendering algorithms, passes, RT
# GPU abstraction lives in gpu/vulkan/

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
)
