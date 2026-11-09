cmake_minimum_required(VERSION 3.20)
project(ohao-engine VERSION 0.1.0)

# Fix git detection for FetchContent
find_package(Git QUIET)
if(NOT GIT_FOUND)
    set(GIT_EXECUTABLE "C:/Program Files/Git/cmd/git.exe")
endif()

# Single-config generators only (Ninja/Make). VS 18 is multi-config — use --config.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
    # The GNU counterpart of the /FORCE:MULTIPLE the MSVC targets link with (docs/notes/platform.md: a second VMA inside
    # NRI_VK): the first definition by link order wins in both. gpu_allocator.cpp keeps NRI's VMA configuration so
    # either copy serves.
    add_link_options(LINKER:--allow-multiple-definition)
    # libstdc++ runs std::execution::par on oneTBB (render/sky/atmosphere.cpp): found here, so every directory sees it.
    find_package(TBB QUIET)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    message(STATUS "MSVC ${MSVC_VERSION} toolset=${CMAKE_VS_PLATFORM_TOOLSET}")
endif()

# Use static CRT (/MT) for consistent linking
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Include FetchContent module
include(FetchContent)

# Tests: every suite, harness and gate is a CTest test with one label (cmake/ohao_tests.cmake).
# `python tools/check.py` is the one entry point that builds and runs them.
enable_testing()
set(Python3_FIND_REGISTRY LAST)   # the `python` on PATH first: the one the harnesses run under
include(${CMAKE_SOURCE_DIR}/cmake/ohao_tests.cmake)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- NRD (NVIDIA RayTracingDenoiser) for realtime RT denoise (Sub-plan 4.A) ----
# Hard dependency when enabled. -DOHAO_NRD=OFF opts out cleanly (no fetch,
# no link, nrd_denoise.cpp is compiled empty via OHAO_NRD_ENABLED guard).
option(OHAO_NRD "Enable NRD (NVIDIA RayTracingDenoiser) integration" ON)

# ---- DLSS Ray Reconstruction (NGX "dlssd") for realtime RT denoise (Phase 5) ----
# Defaults ON only when the SDK's NGX loader is present in the git-ignored external/DLSS
# (python tools/fetch_dlss.py fetches it). -DOHAO_DLSS=OFF opts out cleanly: no include, no
# link, dlss_rr.cpp compiles empty via the OHAO_DLSS_ENABLED guard, and denoise parsing falls
# back to None for "dlssrr".
if(WIN32)
    set(OHAO_DLSS_STATIC_LIB "${CMAKE_SOURCE_DIR}/external/DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
else()
    set(OHAO_DLSS_STATIC_LIB "${CMAKE_SOURCE_DIR}/external/DLSS/lib/Linux_x86_64/libnvsdk_ngx.a")
endif()
if(EXISTS "${OHAO_DLSS_STATIC_LIB}")
    set(OHAO_DLSS_DEFAULT ON)
else()
    set(OHAO_DLSS_DEFAULT OFF)
endif()
option(OHAO_DLSS "Enable NVIDIA DLSS Ray Reconstruction (NGX) integration" ${OHAO_DLSS_DEFAULT})

# Find Vulkan package
find_package(Vulkan REQUIRED)

# FetchContent re-checks every dependency at each configure (a sub-build each, about 0.2 s), and a shader edit
# reconfigures. Every dependency is pinned, so they are checked again only when their declarations change: this file,
# external/cmake/, or the options that choose them. The hash is recorded once a configure has populated them all.
file(GLOB OHAO_DEPENDENCY_FILES ${CMAKE_SOURCE_DIR}/external/cmake/*.cmake ${CMAKE_SOURCE_DIR}/external/CMakeLists.txt)
set(OHAO_DEPENDENCY_TEXT "${OHAO_NRD} ${OHAO_DLSS} ${CMAKE_GENERATOR} ${CMAKE_CXX_COMPILER}")
foreach(f ${CMAKE_SOURCE_DIR}/CMakeLists.txt ${OHAO_DEPENDENCY_FILES})
    file(READ ${f} text)
    string(APPEND OHAO_DEPENDENCY_TEXT "${text}")
endforeach()
string(SHA256 OHAO_DEPENDENCY_HASH "${OHAO_DEPENDENCY_TEXT}")
if(OHAO_DEPENDENCY_HASH STREQUAL OHAO_DEPENDENCIES_POPULATED)
    set(FETCHCONTENT_FULLY_DISCONNECTED ON)
endif()

# Fetch stb (stb_image, stb_image_write, etc.)
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 4e9b368bfc071e97206ea5c6adba68dd0cbf8897   # pinned: a branch is fetched from GitHub at every configure
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(stb)
# The one stb_image implementation (external/stb_image.cpp): the asset loaders and the texture manager both decode images.
add_library(ohao_stb_image STATIC ${CMAKE_SOURCE_DIR}/external/stb_image.cpp)
target_include_directories(ohao_stb_image PUBLIC ${stb_SOURCE_DIR})

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets/models)

# Copy assets
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/models/cornell_box.obj
        ${CMAKE_BINARY_DIR}/assets/models/cornell_box.obj

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/assets/models/cornell_box.mtl
        ${CMAKE_BINARY_DIR}/assets/models/cornell_box.mtl

    COMMENT "Copying assets to build directory"
)



add_subdirectory(external)
add_subdirectory(shaders)
add_subdirectory(ohao)
add_subdirectory(examples)

# OpenPBR's energy-table generator.
add_subdirectory(tools/material)
option(BUILD_SCENE_BENCH "Build scene_bench (scene codec/snapshot/API budgets; CPU only)" ON)
if(BUILD_SCENE_BENCH)
    add_subdirectory(tools/bench)
endif()
# The ohao CLI (validate, fmt, apply, query, summary, run, schema) and its render host.
add_subdirectory(tools/ohao)

# The harnesses and gates that are not one suite's executable (needs every target above).
ohao_register_harness_tests()

# Dependencies point down only (system design 4.1): a link or include up a layer fails configure.
include(cmake/ohao_layers.cmake)
ohao_check_layers()

set(OHAO_DEPENDENCIES_POPULATED ${OHAO_DEPENDENCY_HASH} CACHE INTERNAL "the dependency declarations last populated")

