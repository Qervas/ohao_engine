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

# Model Viewer (with OIDN denoising)
add_executable(model_viewer model_viewer.cpp ${CMAKE_SOURCE_DIR}/ohao/render/rt/denoise/oidn_denoise.cpp)
target_include_directories(model_viewer PRIVATE ${CMAKE_SOURCE_DIR}/ohao ${Vulkan_INCLUDE_DIRS} ${stb_SOURCE_DIR})
target_link_libraries(model_viewer PRIVATE
    -Wl,--start-group ohao_renderer ohao_gpu_vulkan ohao_scene ohao_animation ohao_physics ohao_audio ohao_core -Wl,--end-group
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator OpenImageDenoise)
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

# Interactive Viewer (requires GLFW)
find_package(glfw3 QUIET)
if(glfw3_FOUND)
    add_executable(interactive interactive.cpp)
    target_include_directories(interactive PRIVATE ${CMAKE_SOURCE_DIR}/ohao ${Vulkan_INCLUDE_DIRS} ${stb_SOURCE_DIR})
    find_package(OpenGL REQUIRED)
    target_link_libraries(interactive PRIVATE
        -Wl,--start-group ohao_renderer ohao_gpu_vulkan ohao_scene ohao_animation ohao_physics ohao_audio ohao_core -Wl,--end-group
        ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator glfw OpenGL::GL)
    if(MSVC)
        target_link_options(interactive PRIVATE /FORCE:MULTIPLE)
    else()
        target_link_options(interactive PRIVATE -Wl,--allow-multiple-definition)
    endif()
    if(TARGET Jolt)
        target_link_libraries(interactive PRIVATE Jolt)
    endif()
    message(STATUS "Interactive viewer enabled (GLFW found)")
endif()

# Turntable Recorder (with OIDN)
add_executable(turntable turntable.cpp ${CMAKE_SOURCE_DIR}/ohao/render/rt/denoise/oidn_denoise.cpp)
target_include_directories(turntable PRIVATE ${CMAKE_SOURCE_DIR}/ohao ${Vulkan_INCLUDE_DIRS} ${stb_SOURCE_DIR})
target_link_libraries(turntable PRIVATE
    -Wl,--start-group ohao_renderer ohao_gpu_vulkan ohao_scene ohao_animation ohao_physics ohao_audio ohao_core -Wl,--end-group
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator OpenImageDenoise)
if(MSVC)
    target_link_options(turntable PRIVATE /FORCE:MULTIPLE)
else()
    target_link_options(turntable PRIVATE -Wl,--allow-multiple-definition)
endif()
if(TARGET Jolt)
    target_link_libraries(turntable PRIVATE Jolt)
else()
    message(STATUS "Interactive viewer disabled (GLFW not found)")
endif()
