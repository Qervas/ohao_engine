# A game (steps 3i to 3k): games/<name>, a game::Module, built by ohao_add_game(<name>) from its directory's sources.
#   ohao_<name>_objects  its objects, compiled once: position independent, the game API (game/api.hpp) precompiled,
#                        the engine's headers and definitions taken without its code ($<COMPILE_ONLY:...>)
#   ohao_<name>          the module (libohao_<name>.so; ohao_<name>.dll on Windows) the hosts load at run time
#                        (game/loader.hpp; --game); its references to the engine resolve against the host executable
#   ohao_<name>_static   the same objects for tools that link the game (the benches that measure its internals)
# Each game is in OHAO_GAMES (a global property): cmake/ohao_layers.cmake puts its targets above every engine library
# and holds its sources to the game API (no other engine header). Its objects are compiled with OHAO_GAME_MODULE.
#
# A host (ohao_game_host(<target>)) exports the engine to the games it loads: on Linux every symbol (ENABLE_EXPORTS);
# with MSVC what the games import (ohao_link_games, at the end of the top-level CMakeLists: game/loader.hpp).
function(ohao_add_game name)
    file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
    add_library(ohao_${name}_objects OBJECT ${sources})
    set_target_properties(ohao_${name}_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ohao_${name}_objects PUBLIC ${CMAKE_SOURCE_DIR}/games)
    target_compile_features(ohao_${name}_objects PUBLIC cxx_std_20)
    target_link_libraries(ohao_${name}_objects PUBLIC $<COMPILE_ONLY:ohao_game_api> $<COMPILE_ONLY:ohao_ecs>
        $<COMPILE_ONLY:ohao_world_render> $<COMPILE_ONLY:ohao_reflect>)
    target_compile_definitions(ohao_${name}_objects PRIVATE OHAO_GAME_MODULE)
    # The engine a game sees, parsed once: an edit's rebuild (a reload) parses only the file's own code.
    target_precompile_headers(ohao_${name}_objects PRIVATE <game/api.hpp> <functional> <memory> <string> <vector>)

    if(MSVC)
        # Its engine calls are imports from "ohao_engine.exe", delay-loaded, bound to the host by the hook.
        add_library(ohao_${name} MODULE $<TARGET_OBJECTS:ohao_${name}_objects>
            ${CMAKE_SOURCE_DIR}/ohao/game/module_imports_win.cpp)
        target_link_libraries(ohao_${name} PRIVATE "${OHAO_GAME_IMPORTS_DIR}/ohao_game_imports.lib" delayimp)
        target_link_options(ohao_${name} PRIVATE /DELAYLOAD:ohao_engine.exe)
        set_property(TARGET ohao_${name} APPEND PROPERTY LINK_DEPENDS "${OHAO_GAME_IMPORTS_DIR}/ohao_game_imports.lib")
    else()
        add_library(ohao_${name} MODULE $<TARGET_OBJECTS:ohao_${name}_objects>)
    endif()

    add_library(ohao_${name}_static STATIC $<TARGET_OBJECTS:ohao_${name}_objects>)
    target_include_directories(ohao_${name}_static PUBLIC ${CMAKE_SOURCE_DIR}/games)
    target_compile_features(ohao_${name}_static PUBLIC cxx_std_20)
    target_link_libraries(ohao_${name}_static PUBLIC ohao_game_api ohao_ecs ohao_world_render ohao_reflect)

    set_property(GLOBAL APPEND PROPERTY OHAO_GAMES ${name})
endfunction()

# The games' imports from the engine, for MSVC (per configuration).
set(OHAO_GAME_IMPORTS_DIR "${CMAKE_BINARY_DIR}/game_imports/$<CONFIG>")

# A host that loads games: building it builds the slice beside it.
function(ohao_game_host target)
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(${target} ohao_slice)
    set_property(GLOBAL APPEND PROPERTY OHAO_GAME_HOSTS ${target})
endfunction()

# MSVC: an executable exports only what a .def lists, and the engine defines far past the 65535 names one may export,
# so tools/gen_game_imports.py lists the names the games' objects take from the engine's (the engine libraries a game
# compiles against, and theirs): each host links that .def, and each game the import library made from it. Called once,
# when every target exists.
function(ohao_link_games)
    if(NOT MSVC)
        return()
    endif()
    set(todo ohao_game_api ohao_ecs ohao_world_render ohao_reflect)
    set(libs "")
    while(todo)
        list(POP_FRONT todo t)
        if(t IN_LIST libs)
            continue()
        endif()
        list(APPEND libs ${t})
        get_target_property(direct ${t} LINK_LIBRARIES)
        get_target_property(interface ${t} INTERFACE_LINK_LIBRARIES)
        foreach(l ${direct} ${interface})
            if(l MATCHES "^\\$<LINK_ONLY:(.+)>$")
                set(l ${CMAKE_MATCH_1})
            endif()
            if(l MATCHES "\\$<" OR NOT TARGET ${l})
                continue()
            endif()
            get_target_property(type ${l} TYPE)
            get_target_property(dir ${l} SOURCE_DIR)
            if(type STREQUAL "STATIC_LIBRARY" AND dir MATCHES "^${CMAKE_SOURCE_DIR}/ohao(/|$)")
                list(APPEND todo ${l})
            endif()
        endforeach()
    endwhile()
    set(engine "")
    foreach(l ${libs})
        list(APPEND engine "$<TARGET_OBJECTS:${l}>")
    endforeach()
    get_property(games GLOBAL PROPERTY OHAO_GAMES)
    set(objects "")
    foreach(g ${games})
        list(APPEND objects "$<TARGET_OBJECTS:ohao_${g}_objects>")
    endforeach()
    set(dir "${OHAO_GAME_IMPORTS_DIR}")
    file(GENERATE OUTPUT "${dir}/engine.rsp" CONTENT "$<JOIN:${engine},\n>\n")
    file(GENERATE OUTPUT "${dir}/games.rsp" CONTENT "$<JOIN:${objects},\n>\n")
    add_custom_command(
        OUTPUT "${dir}/imports.stamp"
        BYPRODUCTS "${dir}/ohao_game_imports.def" "${dir}/ohao_game_imports.lib" "${dir}/ohao_game_imports.exp"
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_game_imports.py --engine "${dir}/engine.rsp"
            --games "${dir}/games.rsp" --def "${dir}/ohao_game_imports.def" --lib "${dir}/ohao_game_imports.lib"
            --lib-exe ${CMAKE_AR} --cache "${dir}/symbols.json" --stamp "${dir}/imports.stamp"
        DEPENDS ${libs} ${objects} ${CMAKE_SOURCE_DIR}/tools/gen_game_imports.py "${dir}/engine.rsp" "${dir}/games.rsp"
        COMMENT "The games' imports from the engine"
        VERBATIM)
    add_custom_target(ohao_game_imports DEPENDS "${dir}/imports.stamp")
    foreach(g ${games})
        add_dependencies(ohao_${g} ohao_game_imports)
    endforeach()
    get_property(hosts GLOBAL PROPERTY OHAO_GAME_HOSTS)
    foreach(h ${hosts})
        add_dependencies(${h} ohao_game_imports)
        target_link_options(${h} PRIVATE "/DEF:${dir}/ohao_game_imports.def")
        set_property(TARGET ${h} APPEND PROPERTY LINK_DEPENDS "${dir}/ohao_game_imports.def")
    endforeach()
endfunction()
