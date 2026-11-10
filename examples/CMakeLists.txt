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
            ohao_world_render ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_asset ohao_physics ohao_audio ohao_core
            ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
        target_link_options(${name} PRIVATE /FORCE:MULTIPLE)
    else()
        target_link_libraries(${name} PRIVATE
            -Wl,--start-group ohao_world_render ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_asset ohao_physics ohao_audio ohao_core -Wl,--end-group
            ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
        target_link_options(${name} PRIVATE -Wl,--allow-multiple-definition)
    endif()
    if(TARGET Jolt)
        target_link_libraries(${name} PRIVATE Jolt)
    endif()
    add_dependencies(${name} shaders)   # every example renders: never with stale SPIR-V
endfunction()

ohao_example(cornell_box)
ohao_example(model_viewer)
ohao_example(env_demo)
ohao_example(turntable)


# The windowed game: a scene file played by the live host (ohao_runtime).
ohao_example(ohao_game)
target_link_libraries(ohao_game PRIVATE ohao_runtime ohao_api)
set_target_properties(ohao_game PROPERTIES ENABLE_EXPORTS ON)   # the slice is loaded at run time (game/loader.hpp)
add_dependencies(ohao_game ohao_slice)
