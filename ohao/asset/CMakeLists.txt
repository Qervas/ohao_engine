file(GLOB_RECURSE ASSET_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
file(GLOB_RECURSE ASSET_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# ufbx single-file FBX parser
set(UFBX_SOURCE "${CMAKE_SOURCE_DIR}/external/ufbx.c")

add_library(ohao_asset STATIC ${ASSET_HEADERS} ${ASSET_SOURCES} ${UFBX_SOURCE})

target_include_directories(ohao_asset
    PUBLIC
        ${CMAKE_SOURCE_DIR}/ohao
        ${Vulkan_INCLUDE_DIRS}
)

target_compile_features(ohao_asset PUBLIC cxx_std_20)

target_link_libraries(ohao_asset
    PUBLIC
        ohao_core
        ohao_level   # glTF structure: ModelLoader::loadMesh's primitive ranges
        glm
        ${Vulkan_LIBRARIES}
)

# stb for image decoding (glTF goes through Assimp)
target_include_directories(ohao_asset PRIVATE
    ${stb_SOURCE_DIR}
    ${nlohmann_json_SOURCE_DIR}/include
    ${nlohmann_json_SOURCE_DIR}/single_include
    ${CMAKE_SOURCE_DIR}/external  # ufbx.h
)

# Assimp for FBX/Collada/etc loading
if(TARGET assimp)
    target_link_libraries(ohao_asset PUBLIC assimp)
    target_include_directories(ohao_asset PRIVATE ${assimp_SOURCE_DIR}/include ${assimp_BINARY_DIR}/include)
endif()
