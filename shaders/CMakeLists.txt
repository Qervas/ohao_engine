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
)

# Create output directory
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders/
)

# Find include files for dependency tracking (all subdirectories)
file(GLOB_RECURSE SHADER_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/**/*.glsl"
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
        COMMAND ${GLSLC} -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/includes ${SHADER} -o ${SPIRV}
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

# Optional: Print found shader files
message(STATUS "Found shader files:")
foreach(SHADER ${SHADER_SOURCES})
    message(STATUS "  ${SHADER}")
endforeach()
