# Find glslc compiler
find_program(GLSLC glslc)

if(NOT GLSLC)
    message(FATAL_ERROR "glslc shader compiler not found!")
endif()

# Find all shader files
file(GLOB SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.frag"
)

# Create output directory
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders/
)

# Generate compilation commands for each shader
set(SHADER_BINARY_FILES "")
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPIRV "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPIRV}
        COMMAND ${GLSLC} ${SHADER} -o ${SPIRV}
        DEPENDS ${SHADER} ${CMAKE_BINARY_DIR}/shaders
        COMMENT "Compiling shader ${SHADER_NAME}"
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
