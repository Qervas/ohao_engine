# Input System Library
file(GLOB_RECURSE INPUT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE INPUT_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_library(ohao_input STATIC ${INPUT_SOURCES} ${INPUT_HEADERS})

target_include_directories(ohao_input PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/glfw/include
)

target_link_libraries(ohao_input PUBLIC
    glm
    glfw
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${INPUT_SOURCES} ${INPUT_HEADERS})
