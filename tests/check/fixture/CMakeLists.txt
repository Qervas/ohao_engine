# The selftest fixtures for tools/check.py's own test (tests/check/check_tests.py,
# sub-project 4 test 3). A Python-only project, configured into its own build tree
# (build/check_fixture) so the check under test never shares a Testing/ directory with
# the run that is testing it.
cmake_minimum_required(VERSION 3.21)
project(ohao_check_fixture NONE)
enable_testing()
set(Python3_FIND_REGISTRY LAST)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(fixture ${CMAKE_CURRENT_SOURCE_DIR}/fixture.py)
add_test(NAME fixture_pass COMMAND ${Python3_EXECUTABLE} ${fixture} pass)
add_test(NAME fixture_fail COMMAND ${Python3_EXECUTABLE} ${fixture} fail)
add_test(NAME fixture_other_label COMMAND ${Python3_EXECUTABLE} ${fixture} pass)
set_tests_properties(fixture_pass fixture_fail PROPERTIES LABELS selftest)
set_tests_properties(fixture_other_label PROPERTIES LABELS quick)
# Benchmarks under outside load (exit 2, "noisy"): the check reruns them at most twice.
add_test(NAME fixture_noisy_once COMMAND ${Python3_EXECUTABLE} ${fixture} noisy_once
    ${CMAKE_CURRENT_BINARY_DIR}/noisy_once.runs)
add_test(NAME fixture_noisy_always COMMAND ${Python3_EXECUTABLE} ${fixture} noisy_always
    ${CMAKE_CURRENT_BINARY_DIR}/noisy_always.runs)
set_tests_properties(fixture_noisy_once fixture_noisy_always PROPERTIES LABELS bench)
