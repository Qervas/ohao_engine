# A game (steps 3i to 3k): games/<name>, a game::Module, built by ohao_add_game(<name>) from its directory's sources.
#   ohao_<name>_objects  its objects, compiled once: position independent, the game API (game/api.hpp) precompiled,
#                        the engine's headers and definitions taken without its code ($<COMPILE_ONLY:...>)
#   ohao_<name>          the module (lib<name>.so's name: libohao_<name>.so) the hosts load at run time
#                        (game/loader.hpp; --game); its references to the engine resolve against the host executable
#   ohao_<name>_static   the same objects for tools that link the game (the benches that measure its internals)
# Each game is in OHAO_GAMES (a global property): cmake/ohao_layers.cmake puts its targets above every engine library
# and holds its sources to the game API (no other engine header).
function(ohao_add_game name)
    file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
    add_library(ohao_${name}_objects OBJECT ${sources})
    set_target_properties(ohao_${name}_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ohao_${name}_objects PUBLIC ${CMAKE_SOURCE_DIR}/games)
    target_compile_features(ohao_${name}_objects PUBLIC cxx_std_20)
    target_link_libraries(ohao_${name}_objects PUBLIC $<COMPILE_ONLY:ohao_game_api> $<COMPILE_ONLY:ohao_ecs>
        $<COMPILE_ONLY:ohao_world_render> $<COMPILE_ONLY:ohao_reflect>)
    # The engine a game sees, parsed once: an edit's rebuild (a reload) parses only the file's own code.
    target_precompile_headers(ohao_${name}_objects PRIVATE <game/api.hpp> <functional> <memory> <string> <vector>)

    add_library(ohao_${name} MODULE $<TARGET_OBJECTS:ohao_${name}_objects>)

    add_library(ohao_${name}_static STATIC $<TARGET_OBJECTS:ohao_${name}_objects>)
    target_include_directories(ohao_${name}_static PUBLIC ${CMAKE_SOURCE_DIR}/games)
    target_compile_features(ohao_${name}_static PUBLIC cxx_std_20)
    target_link_libraries(ohao_${name}_static PUBLIC ohao_game_api ohao_ecs ohao_world_render ohao_reflect)

    set_property(GLOBAL APPEND PROPERTY OHAO_GAMES ${name})
endfunction()
