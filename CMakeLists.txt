cmake_minimum_required(VERSION 3.20)
project(ohao-engine VERSION 0.1.0)

# Add debug flags
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find packages
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets/models)

# Symlink assets
add_custom_target(link_assets ALL
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_SOURCE_DIR}/assets/models/cornell_box.obj
        ${CMAKE_BINARY_DIR}/assets/models/cornell_box.obj

    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_SOURCE_DIR}/assets/models/cornell_box.mtl
        ${CMAKE_BINARY_DIR}/assets/models/cornell_box.mtl
)


add_subdirectory(external)
add_subdirectory(shaders)
add_subdirectory(src)

# Add your tests here
file(GLOB TEST_FILES "src/tests/*.cpp")
message(STATUS "Including test files: ${TEST_FILES}")
# Don't include test files in the main engine executable
# target_sources(ohao_engine PRIVATE ${TEST_FILES})

# Optional standalone test executables
# Removed serialization_simple_test due to dependency issues
# add_executable(serialization_simple_test
#   src/tests/serialization_simple_test.cpp
# )
# target_link_libraries(serialization_simple_test ohao_core)

add_executable(serialization_minimal_test
  src/tests/serialization_minimal_test.cpp
)
target_link_libraries(serialization_minimal_test nlohmann_json::nlohmann_json)
