# OHAO Examples — standalone programs that link against the engine

# Cornell Box
add_executable(cornell_box cornell_box.cpp)
target_include_directories(cornell_box PRIVATE ${CMAKE_SOURCE_DIR}/ohao ${Vulkan_INCLUDE_DIRS} ${stb_SOURCE_DIR})
target_link_libraries(cornell_box PRIVATE
    -Wl,--start-group ohao_renderer ohao_gpu_vulkan ohao_scene ohao_animation ohao_physics ohao_audio ohao_core -Wl,--end-group
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
if(MSVC)
    target_link_options(cornell_box PRIVATE /FORCE:MULTIPLE)
else()
    target_link_options(cornell_box PRIVATE -Wl,--allow-multiple-definition)
endif()
if(TARGET Jolt)
    target_link_libraries(cornell_box PRIVATE Jolt)
endif()

# Model Viewer
add_executable(model_viewer model_viewer.cpp)
target_include_directories(model_viewer PRIVATE ${CMAKE_SOURCE_DIR}/ohao ${Vulkan_INCLUDE_DIRS} ${stb_SOURCE_DIR})
target_link_libraries(model_viewer PRIVATE
    -Wl,--start-group ohao_renderer ohao_gpu_vulkan ohao_scene ohao_animation ohao_physics ohao_audio ohao_core -Wl,--end-group
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
if(MSVC)
    target_link_options(model_viewer PRIVATE /FORCE:MULTIPLE)
else()
    target_link_options(model_viewer PRIVATE -Wl,--allow-multiple-definition)
endif()
if(TARGET Jolt)
    target_link_libraries(model_viewer PRIVATE Jolt)
endif()

# Environment Demo
add_executable(env_demo env_demo.cpp)
target_include_directories(env_demo PRIVATE ${CMAKE_SOURCE_DIR}/ohao ${Vulkan_INCLUDE_DIRS} ${stb_SOURCE_DIR})
target_link_libraries(env_demo PRIVATE
    -Wl,--start-group ohao_renderer ohao_gpu_vulkan ohao_scene ohao_animation ohao_physics ohao_audio ohao_core -Wl,--end-group
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
if(MSVC)
    target_link_options(env_demo PRIVATE /FORCE:MULTIPLE)
else()
    target_link_options(env_demo PRIVATE -Wl,--allow-multiple-definition)
endif()
if(TARGET Jolt)
    target_link_libraries(env_demo PRIVATE Jolt)
endif()
