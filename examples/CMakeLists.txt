# The windowed game host (ohao_game), linked against the engine.

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
            ohao_world_render ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_asset ohao_physics ohao_audio_mixer ohao_core
            ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
        target_link_options(${name} PRIVATE /FORCE:MULTIPLE)
    else()
        target_link_libraries(${name} PRIVATE
            -Wl,--start-group ohao_world_render ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_asset ohao_physics ohao_audio_mixer ohao_core -Wl,--end-group
            ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
        target_link_options(${name} PRIVATE -Wl,--allow-multiple-definition)
    endif()
    add_dependencies(${name} shaders)   # every example renders: never with stale SPIR-V
endfunction()

# The windowed game: a scene file played by the live host (ohao_runtime).
ohao_example(ohao_game)
target_link_libraries(ohao_game PRIVATE ohao_runtime ohao_api)
set_target_properties(ohao_game PROPERTIES ENABLE_EXPORTS ON)   # the slice is loaded at run time (game/loader.hpp)
add_dependencies(ohao_game ohao_slice)
