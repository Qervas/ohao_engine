file(GLOB_RECURSE SCENE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
file(GLOB_RECURSE SCENE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# ufbx single-file FBX parser
set(UFBX_SOURCE "${CMAKE_SOURCE_DIR}/external/ufbx.c")

add_library(ohao_scene STATIC ${SCENE_HEADERS} ${SCENE_SOURCES} ${UFBX_SOURCE})

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
    ${CMAKE_SOURCE_DIR}/external  # ufbx.h
)

# Assimp for FBX/Collada/etc loading
if(TARGET assimp)
    target_link_libraries(ohao_scene PUBLIC assimp)
    target_include_directories(ohao_scene PRIVATE ${assimp_SOURCE_DIR}/include ${assimp_BINARY_DIR}/include)
endif()
