file(GLOB_RECURSE COMPONENT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE COMPONENT_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_library(ohao_component STATIC ${COMPONENT_SOURCES} ${COMPONENT_HEADERS})

target_include_directories(ohao_component PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_component PUBLIC
    glm
    nlohmann_json::nlohmann_json
    ${Vulkan_LIBRARIES}
)
