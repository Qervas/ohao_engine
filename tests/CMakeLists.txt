add_executable(engine_tests
    engine_tests.cpp
    multi_object_test.cpp
)

target_link_libraries(engine_tests
    ohao_engine
    ohao_physics
    ohao_renderer
    ohao_ui
    gtest
    gtest_main
    pthread
)

# Add test discovery
include(GoogleTest)
gtest_discover_tests(engine_tests)
