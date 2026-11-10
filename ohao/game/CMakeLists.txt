# The game API (step 3i): what a game is to the engine (game::Module), its methods' rows, loading a module at run time
# (game/loader.hpp) and a game's life on a World (game/install.hpp: install, uninstall, reload; step 3j). A game links it and the engine libraries it calls; ohao_api and the hosts take a
# Module.
add_library(ohao_game_api STATIC module.hpp loader.cpp loader.hpp install.cpp install.hpp)
target_include_directories(ohao_game_api PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_game_api PUBLIC cxx_std_20)
# install.cpp records and removes what a game adds to a World (its systems, hooks, drivers, presenters).
target_link_libraries(ohao_game_api PUBLIC ohao_reflect ohao_ecs ohao_world_render ${CMAKE_DL_LIBS})
