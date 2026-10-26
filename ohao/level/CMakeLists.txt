# ohao_level -- the level pipeline (sub-project 5): a glTF file's structure (nodes, extras,
# lights, cameras, primitive counts) read from its JSON. ohao_scene links it (ModelLoader's
# per-mesh loading needs the primitive counts), so it depends on nothing above core: glm,
# and simdjson privately for the JSON.
file(GLOB LEVEL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB LEVEL_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_level STATIC ${LEVEL_SOURCES} ${LEVEL_HEADERS})
target_include_directories(ohao_level PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_level PUBLIC cxx_std_20)
target_link_libraries(ohao_level PUBLIC ohao_core glm PRIVATE ohao_simdjson)
