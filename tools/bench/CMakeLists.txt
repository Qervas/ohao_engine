add_executable(scene_bench scene_bench.cpp bench_env.hpp bench_world.hpp)
target_compile_features(scene_bench PRIVATE cxx_std_20)
target_link_libraries(scene_bench PRIVATE ohao_api ohao_test_harness)
target_compile_definitions(scene_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(scene_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
    target_link_libraries(scene_bench PRIVATE psapi)
endif()
# The budget gate. Alone (RUN_SERIAL): other tests running beside it would be its noise.
# Exit 2 = noisy run (tools/check.py reruns it).
ohao_add_test(scene_bench LABEL bench SERIAL TIMEOUT 1200
    COMMAND scene_bench --check ${CMAKE_SOURCE_DIR}/tests/bench/scene_budgets.json)

# The protocol's budgets (sub-project 4, step 6): `ohao serve` as a child process on
# pipes, plus the logger's per-call costs. Exit 2 = noisy run (tools/check.py reruns it).
add_executable(serve_bench serve_bench.cpp bench_env.hpp bench_world.hpp)
target_compile_features(serve_bench PRIVATE cxx_std_20)
target_link_libraries(serve_bench PRIVATE ohao_api ohao_test_harness)
target_compile_definitions(serve_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(serve_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
    target_link_libraries(serve_bench PRIVATE psapi)
endif()
add_dependencies(serve_bench ohao)   # the server it drives
ohao_add_test(serve_bench LABEL bench SERIAL TIMEOUT 600
    COMMAND serve_bench --check ${CMAKE_SOURCE_DIR}/tests/bench/serve_budgets.json)
