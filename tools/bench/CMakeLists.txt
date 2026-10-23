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
