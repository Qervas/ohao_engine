file(GLOB_RECURSE GPU_VK_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
file(GLOB_RECURSE GPU_VK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

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
    ohao_core
    glm
    VulkanMemoryAllocator
    ${Vulkan_LIBRARIES}
)

# NRD's NRI_VK.lib carries its own VMA build with the same global symbols. Under
# /FORCE:MULTIPLE whichever object the linker pulls first wins, and NRI's VMA paired
# with our headers crashes in vmaCreateAllocator. Pulling our VMA object up front
# makes every consumer link ours, whatever its library order.
if(MSVC)
    target_link_options(ohao_gpu_vulkan INTERFACE /INCLUDE:vmaCreateAllocator)
endif()
