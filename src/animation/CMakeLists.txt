file(GLOB_RECURSE ANIMATION_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ANIMATION_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_library(ohao_animation STATIC ${ANIMATION_SOURCES} ${ANIMATION_HEADERS})

target_include_directories(ohao_animation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(ohao_animation PUBLIC
    ohao_component
    glm
)
