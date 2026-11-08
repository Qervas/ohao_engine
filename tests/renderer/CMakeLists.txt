# Renderer tests. Temporary (Frank, 2026-11-08): manifold_tests holds caustics step 2's solver while that work
# is built, and goes when it is done. The renderer's lasting gates are the harnesses in cmake/ohao_tests.cmake.
add_executable(manifold_tests manifold_tests.cpp)   # caustics step 2: the manifold solver against its CPU twin
target_link_libraries(manifold_tests PRIVATE ohao_manifold_cpu ohao_gtest_main)
target_include_directories(manifold_tests PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
if(UNIX)
    target_link_libraries(manifold_tests PRIVATE pthread)
endif()
ohao_add_test(manifold_tests LABEL quick TIMEOUT 300 COMMAND manifold_tests)
