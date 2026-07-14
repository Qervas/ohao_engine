# OHAO Shadow System Tests
# Tests handle type safety (compile-time static_asserts) + runtime shadow/light behavior

add_executable(shadow_system_tests
    shadow_system_tests.cpp
)
target_compile_features(shadow_system_tests PRIVATE cxx_std_20)

target_include_directories(shadow_system_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(shadow_system_tests PRIVATE
    ohao_renderer
    ohao_scene
    ${Vulkan_LIBRARIES}
    glm
)

add_test(NAME ShadowSystemTests COMMAND shadow_system_tests)

message(STATUS "Shadow system tests configured")
