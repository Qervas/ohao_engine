file(GLOB_RECURSE TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE TEST_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# Create a list of included files for display purposes
set(INCLUDED_TEST_FILES
    ${TEST_SOURCES}
    ${TEST_HEADERS}
)

# Log included files
foreach(file ${INCLUDED_TEST_FILES})
    message(STATUS "Including test file: ${file}")
endforeach()

# ========== Test Executables ==========

# Physics test executable (removed - building new test for modern collision system)
# add_executable(physics_test
#     physics_test.cpp
# )
# target_link_libraries(physics_test
#     ohao_engine
#     ohao_physics
#     ohao_renderer
#     ohao_ui
#     glm
# )

# Serialization minimal test (existing)
add_executable(serialization_minimal_test
    serialization_minimal_test.cpp
)
target_link_libraries(serialization_minimal_test
    nlohmann_json::nlohmann_json
    glm
)

# Shadow system compile-time and runtime tests
add_executable(shadow_system_tests
    shadow_system/shadow_system_tests.cpp
)
target_include_directories(shadow_system_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/renderer
)
target_link_libraries(shadow_system_tests
    ohao_renderer
    glm
)