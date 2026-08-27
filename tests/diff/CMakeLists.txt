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
