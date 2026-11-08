# Test registration. The gates serve five requirements (Frank, 2026-11-08): never hurt the
# machine, the renderer right against closed forms, a handful of goldens, gameplay replays
# exactly, it builds. Nothing else is a gate; a test is added only when a real bug got through.
# Labels: quick (CPU), gpu (creates a Vulkan device), golden (the render goldens), slow.
# Every test runs from the source root. GPU tests share the resource lock `gpu`, so two
# never overlap.

# The shared harness header (tests/common/harness.hpp) for any suite.
add_library(ohao_test_harness INTERFACE)
target_include_directories(ohao_test_harness INTERFACE ${CMAKE_SOURCE_DIR}/tests)

# GoogleTest suites take this main instead of gtest_main: it prints the harness's JSON line.
add_library(ohao_gtest_main STATIC ${CMAKE_SOURCE_DIR}/tests/common/gtest_main.cpp)
target_link_libraries(ohao_gtest_main PUBLIC gtest ohao_test_harness)

find_package(Python3 COMPONENTS Interpreter)

#   ohao_add_test(<name> LABEL <label> [GPU] [SERIAL] [TIMEOUT <s>] [LOCKS <lock>...]
#                 [ENV <k=v>...] COMMAND <cmd> <args>...)
# LOCKS names other resources the test shares, such as a scratch directory under build/:
# two tests holding the same lock never run at once.
function(ohao_add_test name)
    cmake_parse_arguments(T "GPU;SERIAL" "LABEL;TIMEOUT" "ENV;LOCKS;COMMAND" ${ARGN})
    if(NOT T_LABEL OR NOT T_COMMAND)
        message(FATAL_ERROR "ohao_add_test(${name}): LABEL and COMMAND are required")
    endif()
    add_test(NAME ${name} COMMAND ${T_COMMAND} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(${name} PROPERTIES LABELS ${T_LABEL})
    set(locks ${T_LOCKS})
    if(T_GPU)
        list(APPEND locks gpu)
    endif()
    if(locks)
        set_property(TEST ${name} PROPERTY RESOURCE_LOCK ${locks})
    endif()
    if(T_SERIAL)
        set_property(TEST ${name} PROPERTY RUN_SERIAL TRUE)
    endif()
    if(T_TIMEOUT)
        set_property(TEST ${name} PROPERTY TIMEOUT ${T_TIMEOUT})
    endif()
    if(T_ENV)
        set_property(TEST ${name} PROPERTY ENVIRONMENT ${T_ENV})
    endif()
endfunction()

#   ohao_add_python_test(<name> LABEL <label> [GPU] [SERIAL] [TIMEOUT <s>] [LOCKS <lock>...]
#                        SCRIPT <path> [ARGS ...])
function(ohao_add_python_test name)
    cmake_parse_arguments(P "GPU;SERIAL" "LABEL;TIMEOUT;SCRIPT" "ARGS;LOCKS" ${ARGN})
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "Python 3 not found: test ${name} not registered")
        return()
    endif()
    set(flags)
    if(P_GPU)
        list(APPEND flags GPU)
    endif()
    if(P_SERIAL)
        list(APPEND flags SERIAL)
    endif()
    if(P_TIMEOUT)
        list(APPEND flags TIMEOUT ${P_TIMEOUT})
    endif()
    if(P_LOCKS)
        list(APPEND flags LOCKS ${P_LOCKS})
    endif()
    ohao_add_test(${name} LABEL ${P_LABEL} ${flags}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/${P_SCRIPT} ${P_ARGS})
endfunction()

# The gates that are not one suite's executable. Called once, at the end of the top-level
# CMakeLists, when every target exists.
function(ohao_register_harness_tests)
    # Requirement 1, never hurt the machine: the Vulkan validation layer over a few renders.
    ohao_add_python_test(check_validation LABEL gpu GPU TIMEOUT 300 SCRIPT tests/validation/check_validation.py
        ARGS --json build/check_validation.json)
    # Requirement 2, the renderer right: both renderers against closed-form radiance (a floor under one light;
    # a sky the right way up) and OpenPBR against closed forms (the white furnace, the closed box).
    ohao_add_python_test(check_env_orientation LABEL gpu GPU TIMEOUT 300 SCRIPT tests/renderer/check_env_orientation.py)
    ohao_add_python_test(check_analytic LABEL gpu GPU TIMEOUT 600 SCRIPT tests/renderer/check_analytic.py)
    ohao_add_python_test(check_material LABEL gpu GPU TIMEOUT 900 SCRIPT tests/renderer/check_material.py)
    # Requirement 4, gameplay replays exactly: the play scene's fight, a snapshot restored and replayed, and a
    # new process, reach the same world.hash (headless ohao serve, no GPU).
    ohao_add_python_test(replay_test LABEL quick TIMEOUT 300 LOCKS replay_tmp SCRIPT tests/replay/replay_test.py)
    # Requirement 3, no visual accident: a handful of goldens; diffs go to build/golden_diff/.
    ohao_add_python_test(render_golden LABEL golden GPU TIMEOUT 1800
        SCRIPT tests/golden/render_golden.py ARGS tests/golden/manifest.json --json build/golden_diff/golden.json)

    # Temporary (Frank, 2026-11-08: deleted when their work is done).
    # Caustics step 2's closed forms (with manifold_tests, tests/renderer): out of the default set.
    ohao_add_python_test(check_caustics LABEL slow GPU TIMEOUT 900 SCRIPT tests/renderer/check_caustics.py)
    # The agent interface: `ohao serve` as a process, and a whole MCP session against a hidden window.
    ohao_add_python_test(check_serve LABEL quick TIMEOUT 600 LOCKS serve_tmp SCRIPT tests/serve/check_serve.py)
    ohao_add_python_test(e2e_mcp LABEL gpu GPU TIMEOUT 600 SCRIPT tests/serve/e2e_mcp.py)
    # A real window presenting frames, resized at frame 200, under validation.
    if(TARGET ohao_game)
        ohao_add_test(ohao_game_presentation LABEL gpu GPU TIMEOUT 120 ENV OHAO_VALIDATION=1
            COMMAND ohao_game --frames 600 --resize-at 200)
    endif()
endfunction()
