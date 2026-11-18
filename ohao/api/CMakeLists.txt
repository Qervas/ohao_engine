file(GLOB_RECURSE API_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE API_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_api STATIC ${API_SOURCES} ${API_HEADERS})
target_include_directories(ohao_api PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_api PUBLIC cxx_std_20)
# ohao_game_api: a session serves its game's methods (game::Module) after the engine's. ohao_engine: game.reload
# registers the engine's modules' types beside the new build's.
# The modules it includes (tools/check_layers.py, rule 6): the asset methods' meshes and colliders, the session's
# audio.info and audio.mix, vfx.info, the input's players.
target_link_libraries(ohao_api PUBLIC ohao_ecs ohao_reflect ohao_game_api ohao_engine ohao_unordered_dense
    ohao_visual ohao_physics ohao_audio ohao_vfx)
