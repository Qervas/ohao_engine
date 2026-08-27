# Differentiable renderer tests.
#   diff_unit_tests  -- GoogleTest, CPU logic + bare-instance device queries
#   diff_gpu_probe   -- standalone exe, requires a working device (added in Task 3)

add_executable(diff_unit_tests diff_unit_tests.cpp)
target_include_directories(diff_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_features(diff_unit_tests PRIVATE cxx_std_20)
target_link_libraries(diff_unit_tests PRIVATE
    ohao_diff gtest gtest_main ${Vulkan_LIBRARIES})
if(UNIX)
    target_link_libraries(diff_unit_tests PRIVATE pthread)
endif()
include(GoogleTest)
gtest_discover_tests(diff_unit_tests)

add_executable(diff_gpu_probe diff_gpu_probe.cpp gpu_probe_context.cpp)
target_include_directories(diff_gpu_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_features(diff_gpu_probe PRIVATE cxx_std_20)
target_link_libraries(diff_gpu_probe PRIVATE
    ohao_diff ohao_gpu_vulkan glm VulkanMemoryAllocator ${Vulkan_LIBRARIES})
if(MSVC)
    target_link_options(diff_gpu_probe PRIVATE /FORCE:MULTIPLE)
endif()
add_custom_command(TARGET diff_gpu_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:diff_gpu_probe>/bin/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_BINARY_DIR}/shaders"
        "$<TARGET_FILE_DIR:diff_gpu_probe>/bin/shaders"
    COMMENT "Copying shader SPVs for diff_gpu_probe"
)
