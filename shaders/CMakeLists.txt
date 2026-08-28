# Find glslc compiler
find_program(GLSLC glslc)

if(NOT GLSLC)
    message(FATAL_ERROR "glslc shader compiler not found!")
endif()

# Find all shader files (including subdirectories)
file(GLOB_RECURSE SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.geom"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.tesc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.tese"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rgen"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rmiss"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rchit"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rahit"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rint"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rcall"
)

# Create output directory
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders/
)

# Find include files for dependency tracking (all subdirectories)
file(GLOB_RECURSE SHADER_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/**/*.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/water/*.glsl"
)

# Generate compilation commands for each shader
set(SHADER_BINARY_FILES "")
foreach(SHADER ${SHADER_SOURCES})
    # Get relative path from shaders directory to create unique output names
    file(RELATIVE_PATH SHADER_REL ${CMAKE_CURRENT_SOURCE_DIR} ${SHADER})
    # Replace directory separators with underscores for flat output structure
    string(REPLACE "/" "_" SHADER_FLAT_NAME ${SHADER_REL})
    set(SPIRV "${CMAKE_BINARY_DIR}/shaders/${SHADER_FLAT_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/includes ${SHADER} -o ${SPIRV}
        DEPENDS ${SHADER} ${SHADER_INCLUDES} ${CMAKE_BINARY_DIR}/shaders
        COMMENT "Compiling shader ${SHADER_FLAT_NAME}"
    )
    list(APPEND SHADER_BINARY_FILES ${SPIRV})
endforeach()

# Create shader compilation target
add_custom_target(shaders
    DEPENDS ${SHADER_BINARY_FILES}
    COMMENT "Compiling all shaders"
)

# Task 5: copy compiled SPVs into the runtime output directory ONCE, here,
# instead of each consuming test target (diff_gpu_probe, renderer_test)
# attaching its own POST_BUILD copy_directory into the same destination.
# Two independent copies into the same directory race under a parallel
# (-j8) build and can fail with "Error copying directory ...
# build/Release/bin/shaders". Test targets should add_dependencies(<tgt>
# shaders) and rely on this copy instead of performing their own.
#
# Mirror CMake's own default per-config RUNTIME_OUTPUT_DIRECTORY behavior
# (CMAKE_RUNTIME_OUTPUT_DIRECTORY is set to CMAKE_BINARY_DIR with no
# per-config override in the top-level CMakeLists.txt) so this resolves to
# the same directory executables actually land in, on both multi-config
# generators (e.g. Visual Studio, where CMake appends /$<CONFIG>) and
# single-config generators (e.g. Ninja/Makefiles, where it does not).
get_property(OHAO_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(OHAO_IS_MULTI_CONFIG)
    set(OHAO_SHADER_RUNTIME_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>")
else()
    set(OHAO_SHADER_RUNTIME_DIR "${CMAKE_BINARY_DIR}")
endif()

add_custom_command(TARGET shaders POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OHAO_SHADER_RUNTIME_DIR}/bin/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_BINARY_DIR}/shaders"
        "${OHAO_SHADER_RUNTIME_DIR}/bin/shaders"
    COMMENT "Copying shader SPVs to runtime output directory"
)

# Optional: Print found shader files
message(STATUS "Found shader files:")
foreach(SHADER ${SHADER_SOURCES})
    message(STATUS "  ${SHADER}")
endforeach()
