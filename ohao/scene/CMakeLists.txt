file(GLOB_RECURSE SCENE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
file(GLOB_RECURSE SCENE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(ohao_scene STATIC ${SCENE_HEADERS} ${SCENE_SOURCES})

target_include_directories(ohao_scene
    PUBLIC ${CMAKE_SOURCE_DIR}/ohao
)

target_link_libraries(ohao_scene
    PUBLIC
        ohao_core
        glm
        ${Vulkan_LIBRARIES}
)

# tinygltf + stb for asset loading
if(TARGET tinygltf)
    target_link_libraries(ohao_scene PUBLIC tinygltf)
endif()
target_include_directories(ohao_scene PRIVATE
    ${stb_SOURCE_DIR}
    ${tinygltf_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
    ${nlohmann_json_SOURCE_DIR}/single_include
)
