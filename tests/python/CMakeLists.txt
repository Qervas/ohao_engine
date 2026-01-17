cmake_minimum_required(VERSION 3.15)

# Find Python and pybind11
find_package(Python COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 CONFIG)

if(NOT pybind11_FOUND)
    message(STATUS "pybind11 not found, fetching from GitHub...")
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.11.1
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# ===== Physics Python Module =====
pybind11_add_module(ohao_physics_py physics_bindings.cpp)

target_link_libraries(ohao_physics_py PRIVATE
    ohao_physics
    ohao_engine
    ohao_component
    ohao_renderer
    ohao_utils
    ohao_ui
)

set_target_properties(ohao_physics_py PROPERTIES OUTPUT_NAME "ohao_physics")

target_include_directories(ohao_physics_py PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/glm
)

set_target_properties(ohao_physics_py PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ===== Renderer Python Module =====
pybind11_add_module(ohao_renderer_py renderer_bindings.cpp)

target_link_libraries(ohao_renderer_py PRIVATE
    ohao_renderer
    ohao_engine
    ohao_component
    ohao_utils
    ${Vulkan_LIBRARIES}
)

set_target_properties(ohao_renderer_py PROPERTIES OUTPUT_NAME "ohao_renderer")

target_include_directories(ohao_renderer_py PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/glm
    ${Vulkan_INCLUDE_DIRS}
)

set_target_properties(ohao_renderer_py PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Copy test files to build directory
file(COPY
    test_physics_invariants.py
    test_analytical_solutions.py
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}"
)

message(STATUS "Python bindings configured.")
message(STATUS "  Physics module: make ohao_physics_py")
message(STATUS "  Renderer module: make ohao_renderer_py")
