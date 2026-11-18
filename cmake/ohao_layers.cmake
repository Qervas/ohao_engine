# The engine's layers and modules (system design 4.1, step 2g; docs/specs/2026-11-18-engine-modules-design.md, 5.2
# and 5.3): dependencies point down only, and a module reads another only through the public headers of a module it
# declares. CMake is the one source of the graph: this computes each library's level and writes it, with each
# library's sources and links and each module's DEPENDS, to build/check/modules.json; tools/check_layers.py, the one
# implementation of the scan, holds the sources to it, at configure (which fails) and on every tools/check.py run
# (an include added without a reconfigure is refused too).
#
# A known upward edge is listed in OHAO_LAYER_EXCEPTIONS with the step that removes it, and nowhere else.

# The libraries below the modules, by level.
set(OHAO_LAYERS_BELOW
    ohao_core=0
    ohao_gpu_vulkan=1 ohao_reflect=1 ohao_level=1 ohao_anim=1 ohao_audio_bank=1 ohao_vfx=1 ohao_physics=1
    ohao_asset=2 ohao_audio=2
    ohao_cook=3
    ohao_renderer=4)
# The modules (cmake/ohao_modules.cmake): the core, ecs, at 4 beside the renderer (neither reaches the other; world_render
# joins them), each other one above its highest dependency. Never typed: computed from DEPENDS.
set(OHAO_CORE_LEVEL 4)
# Above the modules, each tier one above the last: the libraries not yet modules, the module list and the game API,
# the agents' API, the hosts. The games (cmake/ohao_games.cmake) sit above them all.
set(OHAO_LAYERS_ABOVE
    "ohao_level_world ohao_world_render"
    "ohao_engine ohao_game_api"
    "ohao_api"
    "ohao_rpc ohao_runtime")

# "from->to": allowed although it points sideways or up. Each has its end.
#   ohao_ecs->ohao_visual: physics, animation, the audio schedule and vfx, still in ohao_ecs, read the visual module's
#   components and primitive meshes; it ends when vfx, the last of them, leaves ohao_ecs (step 4b-6b, commit 8).
set(OHAO_LAYER_EXCEPTIONS ohao_ecs->ohao_visual)

# Each module's level: OHAO_CORE_LEVEL without DEPENDS, else one above its highest dependency. A cycle never settles
# (tools/check_layers.py prints it); the levels then stop after as many passes as there are modules.
function(_ohao_module_levels out_levels out_top)
    get_property(modules GLOBAL PROPERTY OHAO_MODULES)
    foreach(m ${modules})
        set(level_${m} ${OHAO_CORE_LEVEL})
    endforeach()
    list(LENGTH modules n)
    foreach(pass RANGE ${n})
        set(changed FALSE)
        foreach(m ${modules})
            get_property(deps GLOBAL PROPERTY OHAO_MODULE_DEPENDS_${m})
            foreach(d ${deps})
                if(DEFINED level_${d})
                    math(EXPR above "${level_${d}} + 1")
                    if(above GREATER level_${m})
                        set(level_${m} ${above})
                        set(changed TRUE)
                    endif()
                endif()
            endforeach()
        endforeach()
        if(NOT changed)
            break()
        endif()
    endforeach()
    set(levels "")
    set(top ${OHAO_CORE_LEVEL})
    foreach(m ${modules})
        list(APPEND levels ohao_${m}=${level_${m}})
        if(level_${m} GREATER top)
            set(top ${level_${m}})
        endif()
    endforeach()
    set(${out_levels} "${levels}" PARENT_SCOPE)
    set(${out_top} ${top} PARENT_SCOPE)
endfunction()

function(_ohao_json_list out)
    set(text "")
    foreach(item ${ARGN})
        if(text)
            string(APPEND text ",")
        endif()
        string(APPEND text "\"${item}\"")
    endforeach()
    set(${out} "[${text}]" PARENT_SCOPE)
endfunction()

function(ohao_check_layers)
    _ohao_module_levels(module_levels top)
    set(layers ${OHAO_LAYERS_BELOW} ${module_levels})
    set(level ${top})
    foreach(tier ${OHAO_LAYERS_ABOVE})
        math(EXPR level "${level} + 1")
        string(REPLACE " " ";" libs "${tier}")
        foreach(lib ${libs})
            list(APPEND layers ${lib}=${level})
        endforeach()
    endforeach()
    math(EXPR game_level "${level} + 1")
    get_property(games GLOBAL PROPERTY OHAO_GAMES)
    foreach(game ${games})
        list(APPEND layers ohao_${game}_objects=${game_level} ohao_${game}=${game_level} ohao_${game}_static=${game_level})
    endforeach()

    # build/check/modules.json: {root, libraries: {name: {level, sources, links, module?}}, modules: {name: [depends]},
    # games: {name: objects target}, engineDirs, exceptions}.
    set(libraries "")
    foreach(entry ${layers})
        string(REPLACE "=" ";" kv "${entry}")
        list(GET kv 0 lib)
        list(GET kv 1 lv)
        if(NOT TARGET ${lib})
            message(FATAL_ERROR "ohao_layers: ${lib} has a level but is not a target")
        endif()
        get_target_property(type ${lib} TYPE)
        set(links "")
        set(sources "")
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(l ${lib} LINK_LIBRARIES)
            if(l)
                list(APPEND links ${l})
            endif()
            get_target_property(srcs ${lib} SOURCES)
            get_target_property(dir ${lib} SOURCE_DIR)
            foreach(s ${srcs})
                if(s MATCHES "^\\$<")
                    continue()
                endif()
                if(NOT IS_ABSOLUTE "${s}")
                    set(s "${dir}/${s}")
                endif()
                if(s MATCHES "\\.(cpp|hpp|h)$" AND s MATCHES "^${CMAKE_SOURCE_DIR}/")
                    file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}" "${s}")
                    list(APPEND sources "${rel}")
                endif()
            endforeach()
        endif()
        get_target_property(il ${lib} INTERFACE_LINK_LIBRARIES)
        if(il)
            list(APPEND links ${il})
        endif()
        set(leveled "")
        foreach(d ${links})
            string(REGEX REPLACE "^\\$<COMPILE_ONLY:(.*)>$" "\\1" d "${d}")
            if(TARGET ${d} AND NOT d STREQUAL lib)
                list(APPEND leveled ${d})
            endif()
        endforeach()
        list(REMOVE_DUPLICATES leveled)
        _ohao_json_list(jl ${leveled})
        _ohao_json_list(js ${sources})
        if(libraries)
            string(APPEND libraries ",\n    ")
        endif()
        string(APPEND libraries "\"${lib}\": {\"level\": ${lv}, \"links\": ${jl}, \"sources\": ${js}}")
    endforeach()
    get_property(modules GLOBAL PROPERTY OHAO_MODULES)
    set(mods "")
    foreach(m ${modules})
        get_property(deps GLOBAL PROPERTY OHAO_MODULE_DEPENDS_${m})
        _ohao_json_list(jd ${deps})
        if(mods)
            string(APPEND mods ", ")
        endif()
        string(APPEND mods "\"${m}\": ${jd}")
    endforeach()
    set(gs "")
    foreach(game ${games})
        if(gs)
            string(APPEND gs ", ")
        endif()
        string(APPEND gs "\"${game}\": \"ohao_${game}_objects\"")
    endforeach()
    file(GLOB engine_dirs LIST_DIRECTORIES true RELATIVE "${CMAKE_SOURCE_DIR}/ohao" "${CMAKE_SOURCE_DIR}/ohao/*")
    set(dirs "")
    foreach(d ${engine_dirs})
        if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/ohao/${d}")
            list(APPEND dirs ${d})
        endif()
    endforeach()
    _ohao_json_list(jdirs ${dirs})
    _ohao_json_list(jex ${OHAO_LAYER_EXCEPTIONS})
    string(CONCAT json "{\n  \"root\": \"${CMAKE_SOURCE_DIR}\",\n  \"libraries\": {\n    ${libraries}\n  },\n"
                       "  \"modules\": {${mods}},\n  \"games\": {${gs}},\n  \"engineDirs\": ${jdirs},\n"
                       "  \"exceptions\": ${jex}\n}\n")
    set(file "${CMAKE_BINARY_DIR}/check/modules.json")
    set(old "")
    if(EXISTS "${file}")
        file(READ "${file}" old)
    endif()
    if(NOT old STREQUAL json)
        file(WRITE "${file}" "${json}")
    endif()

    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "ohao_layers: the layer check (tools/check_layers.py) needs Python 3")
    endif()
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_layers.py "${file}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "ohao_layers: a dependency points up a layer or across an undeclared module "
                            "(cmake/ohao_layers.cmake, tools/check_layers.py):\n${out}${err}")
    endif()
endfunction()
