# The game API (step 3i): what a game is to the engine (game::Module), its methods' rows, and loading a module at run
# time (game/loader.hpp, step 3j). A game links it and the engine libraries it calls; ohao_api and the hosts take a
# Module.
add_library(ohao_game_api STATIC module.hpp loader.cpp loader.hpp)
target_include_directories(ohao_game_api PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_game_api PUBLIC cxx_std_20)
target_link_libraries(ohao_game_api PUBLIC ohao_reflect ${CMAKE_DL_LIBS})
