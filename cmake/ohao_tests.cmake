# Test registration (sub-project 4, step 2). Every suite, harness and gate is a CTest test
# with exactly one primary label; `python tools/check.py` and plain `ctest -L <label>`
# select by it:
#
#   quick     CPU suites and CPU harnesses (seconds)
#   gpu       suites and harnesses that create a Vulkan device
#   docs      the doc-command checker and the monograph checks
#   golden    the render goldens (minutes; a GPU)
#   bench     the benchmark gates (run alone)
#   slow      exhaustive checks (minutes)
#   selftest  fixtures of tools/check.py's own test (never part of a normal run)
#
# Every test runs from the source root, as the docs' commands do. GPU tests share the
# resource lock `gpu`, so two never overlap; benchmarks are RUN_SERIAL, so nothing runs
# beside them.

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

# The harnesses and gates that are not one suite's executable. Called once, at the end of
# the top-level CMakeLists, when every target exists.
function(ohao_register_harness_tests)
    # The CLI check writes build/cli_tmp; it runs as check_cli_cpu, check_cli and inside
    # check_docs, so those three hold the lock `cli_tmp`.
    # CPU harnesses
    ohao_add_python_test(check_cli_cpu LABEL quick LOCKS cli_tmp SCRIPT tests/cli/check_cli.py ARGS --cpu-only)
    ohao_add_python_test(check_scene_schema LABEL quick SCRIPT tests/reflect/check_scene_schema.py)
    # tools/check.py's own test (test 3): runs the selftest fixtures in their own build tree,
    # build/check_fixture (also run inside check_docs: the lock `check_fixture`).
    ohao_add_python_test(check_tests LABEL quick TIMEOUT 600 LOCKS check_fixture SCRIPT tests/check/check_tests.py)
    # The golden harness's diff report (test 4) and `ohao doctor` (test 5).
    ohao_add_python_test(test_golden_diff LABEL quick SCRIPT tests/golden/test_golden_diff.py)
    ohao_add_python_test(check_doctor LABEL quick SCRIPT tests/doctor/check_doctor.py)
    ohao_add_python_test(check_doctor_gpu LABEL gpu GPU SCRIPT tests/doctor/check_doctor.py ARGS --gpu)
    # The shared shader/C++ layout (sub-project 2b, step 6): generated files current, std430.
    ohao_add_python_test(check_layout LABEL quick SCRIPT tests/layout/check_layout.py)
    # The level pipeline's checked-in assets (sub-project 5, test 1): hints, the blockout's
    # contract, its byte-identical Blender rebuild (skip without Blender).
    ohao_add_python_test(check_level_assets LABEL quick TIMEOUT 600 SCRIPT tests/level/check_level_assets.py)
    # `ohao serve` as a process (tests 8-10, 15): its goldens write build/serve_tmp/<session>.
    ohao_add_python_test(check_serve LABEL quick TIMEOUT 600 LOCKS serve_tmp SCRIPT tests/serve/check_serve.py)

    # GPU harnesses and the presentation gate
    ohao_add_python_test(check_cli LABEL gpu GPU LOCKS cli_tmp SCRIPT tests/cli/check_cli.py)
    ohao_add_python_test(check_validation LABEL gpu GPU SCRIPT tests/validation/check_validation.py
        ARGS --json build/check_validation.json)
    if(TARGET ohao_game)
        ohao_add_test(ohao_game_presentation LABEL gpu GPU ENV OHAO_VALIDATION=1
            COMMAND ohao_game --frames 600 --resize-at 200)
    endif()
    # The live host (sub-project 4, tests 16 and 17): ohao serve --window driven end to end.
    ohao_add_python_test(check_live LABEL gpu GPU TIMEOUT 600 SCRIPT tests/serve/check_live.py)
    # The end-to-end MCP acceptance session (sub-project 4, test 19).
    ohao_add_python_test(e2e_mcp LABEL gpu GPU TIMEOUT 600 SCRIPT tests/serve/e2e_mcp.py)

    # Long gates
    # Diff images for failing scenes and the per-scene JSON go to build/golden_diff/.
    ohao_add_python_test(render_golden LABEL golden GPU TIMEOUT 1800
        SCRIPT tests/golden/render_golden.py ARGS tests/golden/manifest.json --json build/golden_diff/golden.json)
    if(TARGET reflect_tests)
        ohao_add_test(reflect_exhaustive_floats LABEL slow TIMEOUT 1800
            COMMAND reflect_tests --exhaustive-floats)
    endif()

    # Docs
    ohao_add_python_test(check_docs LABEL docs TIMEOUT 1200 LOCKS cli_tmp check_fixture serve_tmp
        SCRIPT tests/docs/check_docs.py ARGS --cpu-only)
    ohao_add_python_test(monograph_structure LABEL docs SCRIPT site/tests/test_monograph_structure.py)
endfunction()
