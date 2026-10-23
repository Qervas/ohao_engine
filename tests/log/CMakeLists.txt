add_executable(log_tests log_tests.cpp)
target_compile_features(log_tests PRIVATE cxx_std_20)
target_link_libraries(log_tests PRIVATE ohao_core ohao_test_harness)
ohao_add_test(log_tests LABEL quick COMMAND log_tests)
