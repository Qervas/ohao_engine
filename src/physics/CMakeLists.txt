file(GLOB_RECURSE PHYSICS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE PHYSICS_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_library(ohao_physics STATIC ${PHYSICS_SOURCES} ${PHYSICS_HEADERS})

target_include_directories(ohao_physics PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/imgui
    ${CMAKE_SOURCE_DIR}/external/imgui/backends
)

target_link_libraries(ohao_physics PUBLIC
    glm
    imgui
    nlohmann_json::nlohmann_json
) 