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

target_compile_features(ohao_gpu_vulkan PUBLIC cxx_std_20)

target_link_libraries(ohao_gpu_vulkan PUBLIC
    ohao_scene
    ohao_core
    glm
    VulkanMemoryAllocator
    ${Vulkan_LIBRARIES}
)

# Phase 5 — DLSS. device_setup.cpp adds the NGX-required instance/device
# extensions under OHAO_DLSS_ENABLED, and this lib instantiates PathTracer's
# layout (via rt_profile_renderer.hpp) which carries an OHAO_DLSS_ENABLED-guarded
# member. Defining the macro here keeps that layout ODR-consistent with
# ohao_renderer. No NGX headers/link needed here — device_setup uses only Vulkan
# extension-name macros; the NGX calls live entirely in ohao_renderer.
if(OHAO_DLSS AND TARGET ngx_dlss)
    target_compile_definitions(ohao_gpu_vulkan PRIVATE OHAO_DLSS_ENABLED)
endif()
