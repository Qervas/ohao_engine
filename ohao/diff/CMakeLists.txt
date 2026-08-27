# OHAO Differentiable Renderer
# Stage 0a: scaffolding only -- no gradients are computed here yet.

file(GLOB_RECURSE DIFF_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

add_library(ohao_diff STATIC ${DIFF_SOURCES})

target_include_directories(ohao_diff
    PUBLIC
        ${CMAKE_SOURCE_DIR}/ohao
        ${Vulkan_INCLUDE_DIRS}
)

target_compile_features(ohao_diff PUBLIC cxx_std_20)

target_link_libraries(ohao_diff PUBLIC
    ohao_gpu_vulkan
    glm
    VulkanMemoryAllocator
    ${Vulkan_LIBRARIES}
)
