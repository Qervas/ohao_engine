add_executable(scene_bench scene_bench.cpp)
target_compile_features(scene_bench PRIVATE cxx_std_20)
target_link_libraries(scene_bench PRIVATE ohao_api)
target_compile_definitions(scene_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(scene_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
# The budget gate. Alone (RUN_SERIAL): other tests running beside it would be its noise.
ohao_add_test(scene_bench LABEL bench SERIAL TIMEOUT 1200
    COMMAND scene_bench --check ${CMAKE_SOURCE_DIR}/tests/bench/scene_budgets.json)

# The logger's budgets (sub-project 4, step 4); exit 2 = noisy run.
add_executable(log_bench log_bench.cpp bench_env.hpp)
target_compile_features(log_bench PRIVATE cxx_std_20)
target_link_libraries(log_bench PRIVATE ohao_core ohao_test_harness)
ohao_add_test(log_bench LABEL bench SERIAL TIMEOUT 300
    COMMAND log_bench --check ${CMAKE_SOURCE_DIR}/tests/bench/serve_budgets.json)
