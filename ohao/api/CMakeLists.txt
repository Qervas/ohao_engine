file(GLOB_RECURSE API_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE API_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_api STATIC ${API_SOURCES} ${API_HEADERS})
target_include_directories(ohao_api PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_api PUBLIC cxx_std_20)
# ohao_level_world: level.info and level.reload read the level system's summary and reload it
# (level/level_system.hpp). It links level, ecs and reflect only: no cycle back to the API.
# ohao_game_api: a session serves its game's methods (game::Module) after the engine's.
target_link_libraries(ohao_api PUBLIC ohao_ecs ohao_level_world ohao_reflect ohao_game_api ohao_unordered_dense)
