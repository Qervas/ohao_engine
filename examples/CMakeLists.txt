# OHAO Examples — standalone programs that link against the engine

function(ohao_example name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/ohao
        ${CMAKE_SOURCE_DIR}/examples
        ${Vulkan_INCLUDE_DIRS}
        ${stb_SOURCE_DIR})
    target_compile_features(${name} PRIVATE cxx_std_20)
    if(MSVC)
        target_link_libraries(${name} PRIVATE
            ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
            ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
        target_link_options(${name} PRIVATE /FORCE:MULTIPLE)
    else()
        target_link_libraries(${name} PRIVATE
            -Wl,--start-group ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core -Wl,--end-group
            ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
        target_link_options(${name} PRIVATE -Wl,--allow-multiple-definition)
    endif()
    if(TARGET Jolt)
        target_link_libraries(${name} PRIVATE Jolt)
    endif()
endfunction()

ohao_example(cornell_box)
ohao_example(model_viewer)
ohao_example(env_demo)
ohao_example(turntable)


# Interactive Viewer. GLFW is built from source by external/cmake/glfw.cmake
# so it matches the project's CRT; no find_package or prebuilt binary needed.
ohao_example(interactive)
find_package(OpenGL REQUIRED)
target_link_libraries(interactive PRIVATE glfw OpenGL::GL)
