# OHAO Renderer Standalone Tests
# Run the Vulkan renderer WITHOUT Godot — pure C++ verification.

# Standalone renderer test — builds scene, renders to PNG
add_executable(renderer_test renderer_test.cpp)

target_include_directories(renderer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
)

target_link_libraries(renderer_test PRIVATE
    ohao_renderer
    ohao_engine
    ohao_scene
    ohao_actor
    ohao_asset
    ohao_animation
    ohao_component
    ohao_physics
    ohao_audio
    ohao_ui
    ohao_utils
    ${Vulkan_LIBRARIES}
    glm
    VulkanMemoryAllocator
)

if(TARGET Jolt)
    target_link_libraries(renderer_test PRIVATE Jolt)
endif()

# Allow duplicate stbi symbols (model_gltf.cpp defines STB_IMAGE_IMPLEMENTATION)
if(MSVC)
    target_link_options(renderer_test PRIVATE /FORCE:MULTIPLE)
endif()

# Copy shaders next to exe so renderer can find them
add_custom_command(TARGET renderer_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:renderer_test>/bin/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_BINARY_DIR}/shaders"
        "$<TARGET_FILE_DIR:renderer_test>/bin/shaders"
    COMMENT "Copying shader SPVs for standalone renderer test"
)

message(STATUS "Standalone renderer test configured")
