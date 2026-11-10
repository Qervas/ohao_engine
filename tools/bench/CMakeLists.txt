# Benchmarks: tools run by hand (`<bench> --check tests/bench/<x>_budgets.json`), not CTest gates (Frank, 2026-11-08).
add_executable(scene_bench scene_bench.cpp bench_env.hpp bench_world.hpp)
target_compile_features(scene_bench PRIVATE cxx_std_20)
target_link_libraries(scene_bench PRIVATE ohao_api ohao_test_harness)
target_compile_definitions(scene_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(scene_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
    target_link_libraries(scene_bench PRIVATE psapi)
endif()

# The protocol's budgets (sub-project 4, step 6): `ohao serve` as a child process on
# pipes, plus the logger's per-call costs. Exit 2 = a noisy run (outside load): no verdict.
add_executable(serve_bench serve_bench.cpp bench_env.hpp bench_world.hpp)
target_compile_features(serve_bench PRIVATE cxx_std_20)
target_link_libraries(serve_bench PRIVATE ohao_api ohao_test_harness)
target_compile_definitions(serve_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(serve_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
    target_link_libraries(serve_bench PRIVATE psapi)
endif()
add_dependencies(serve_bench ohao)   # the server it drives

# The level pipeline's budgets (sub-project 5, step 4: blockout_load_ms; the kit2k budgets come
# with step 6). CPU, headless. Exit 2 = a noisy run (outside load): no verdict.
add_executable(level_bench level_bench.cpp bench_env.hpp)
target_compile_features(level_bench PRIVATE cxx_std_20)
target_link_libraries(level_bench PRIVATE ohao_api ohao_level_world ohao_ecs ohao_physics ohao_test_harness)
target_compile_definitions(level_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(level_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
    target_link_libraries(level_bench PRIVATE psapi)
endif()

# The physics step's budgets (sub-project 6, step 1): the blockout's drop and rest, 1,000 events
# in a step. CPU, headless. Exit 2 = a noisy run (outside load): no verdict.
add_executable(physics_bench physics_bench.cpp bench_env.hpp)
target_compile_features(physics_bench PRIVATE cxx_std_20)
target_link_libraries(physics_bench PRIVATE ohao_level_world ohao_ecs ohao_physics ohao_runtime ohao_test_harness ohao_slice)
target_compile_definitions(physics_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(physics_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
    target_link_libraries(physics_bench PRIVATE psapi)
endif()

# The mixer's and the schedule's budgets (sub-project 11: the mix, the reverb, the schedule's step, the
# player's submission, the bank's load). CPU, headless. Exit 2 = a noisy run (outside load): no verdict.
add_executable(audio_bench audio_bench.cpp bench_env.hpp)
target_compile_features(audio_bench PRIVATE cxx_std_20)
target_link_libraries(audio_bench PRIVATE ohao_runtime ohao_level_world ohao_ecs ohao_physics ohao_audio ohao_core
                      ohao_test_harness nlohmann_json::nlohmann_json ohao_slice)
target_compile_definitions(audio_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(audio_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
    target_link_libraries(audio_bench PRIVATE psapi)
endif()

# The renderer's GPU budgets (sub-project 2b, step 1): headless captures of fixed scenes,
# per-pass timestamps, a GPU calibration kernel. Exit 2 = a noisy run (outside load): no verdict.
add_executable(render_bench render_bench.cpp bench_env.hpp gpu_calibration.hpp)
target_compile_features(render_bench PRIVATE cxx_std_20)
target_link_libraries(render_bench PRIVATE ohao_host ohao_test_harness ohao_slice)
target_compile_definitions(render_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(render_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
    target_link_libraries(render_bench PRIVATE psapi)
endif()
add_dependencies(render_bench shaders)

# Input-to-present latency and frame pacing (sub-project 9): a visible window plays the play scene
# for a scripted minute per present mode. Exit 1 = a gated budget missed or nothing measured.
add_executable(feel_bench feel_bench.cpp)
target_compile_features(feel_bench PRIVATE cxx_std_20)
target_link_libraries(feel_bench PRIVATE ohao_host ohao_test_harness ohao_slice)
target_compile_definitions(feel_bench PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(feel_bench PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(feel_bench shaders)
