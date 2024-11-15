# Find glslc compiler
find_program(GLSLC glslc)

if(NOT GLSLC)
    message(FATAL_ERROR "glslc shader compiler not found!")
endif()

# Create shader build target
add_custom_target(shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders/
    COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/shader.vert -o ${CMAKE_BINARY_DIR}/shaders/shader.vert.spv
    COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/shader.frag -o ${CMAKE_BINARY_DIR}/shaders/shader.frag.spv
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/shader.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shader.frag
    COMMENT "Compiling shaders"
)
