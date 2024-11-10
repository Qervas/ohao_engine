add_executable(engine_tests
    engine_tests.cpp
)

target_link_libraries(engine_tests
    ohao_core
    ohao_renderer
)
