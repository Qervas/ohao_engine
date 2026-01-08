cmake_minimum_required(VERSION 3.20)
project(ohao-engine VERSION 0.1.0)

# Fix git detection for FetchContent
find_package(Git QUIET)
if(NOT GIT_FOUND)
    set(GIT_EXECUTABLE "C:/Program Files/Git/cmd/git.exe")
endif()

# Add debug flags
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include FetchContent module
include(FetchContent)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Vulkan package
find_package(Vulkan REQUIRED)

# Fetch stb (stb_image, stb_image_write, etc.)
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG master
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(stb)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets/models)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets/fonts)

# Copy assets
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/models/cornell_box.obj
        ${CMAKE_BINARY_DIR}/assets/models/cornell_box.obj

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/models/cornell_box.mtl
        ${CMAKE_BINARY_DIR}/assets/models/cornell_box.mtl

    # Copy FontAwesome font for UI icons
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/fonts/fa-solid-900.ttf
        ${CMAKE_BINARY_DIR}/assets/fonts/fa-solid-900.ttf

    COMMENT "Copying assets and fonts to build directory"
)


# Set explicit GLFW paths
set(GLFW_INCLUDE_DIR "$ENV{USERPROFILE}/scoop/apps/glfw/current/include")
set(GLFW_LIBRARY "$ENV{USERPROFILE}/scoop/apps/glfw/current/lib-vc2022/glfw3.lib")

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

# Tests are now managed in src/tests/CMakeLists.txt
