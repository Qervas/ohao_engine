file(GLOB_RECURSE SERIALIZATION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/map_io.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/map_io.hpp"
)

add_library(ohao_serialization STATIC ${SERIALIZATION_SOURCES})

target_include_directories(ohao_serialization PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_serialization PUBLIC
    nlohmann_json::nlohmann_json
    glm
    ${Vulkan_LIBRARIES}
) 