file(GLOB_RECURSE ASSET_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ASSET_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_library(ohao_asset STATIC ${ASSET_SOURCES} ${ASSET_HEADERS})

target_include_directories(ohao_asset PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_asset PUBLIC
    glm
    ${Vulkan_LIBRARIES}
) 