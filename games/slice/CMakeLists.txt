# The slice (step 3i): the game the engine is built with, a game::Module (slice/module.hpp). Its sources include the
# engine's headers as "ecs/...", its own as "slice/...". Compiled once (position independent), linked two ways:
#   ohao_slice         the module (step 3j): a shared library the hosts load at run time (game/loader.hpp). It links
#                      no engine library: its references to the engine resolve against the host executable, which
#                      exports its symbols (ENABLE_EXPORTS), so there is one engine in the process.
#   ohao_slice_static  the same objects for the benches that measure the slice's internals (they link the engine).
file(GLOB SLICE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_slice_objects OBJECT ${SLICE_SOURCES})
set_target_properties(ohao_slice_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ohao_slice_objects PUBLIC ${CMAKE_SOURCE_DIR}/games)
target_compile_features(ohao_slice_objects PUBLIC cxx_std_20)
# The engine's headers and definitions, never its code (COMPILE_ONLY links nothing).
target_link_libraries(ohao_slice_objects PUBLIC $<COMPILE_ONLY:ohao_game_api> $<COMPILE_ONLY:ohao_ecs>
    $<COMPILE_ONLY:ohao_world_render> $<COMPILE_ONLY:ohao_reflect>)

add_library(ohao_slice MODULE $<TARGET_OBJECTS:ohao_slice_objects>)

add_library(ohao_slice_static STATIC $<TARGET_OBJECTS:ohao_slice_objects>)
target_include_directories(ohao_slice_static PUBLIC ${CMAKE_SOURCE_DIR}/games)
target_compile_features(ohao_slice_static PUBLIC cxx_std_20)
target_link_libraries(ohao_slice_static PUBLIC ohao_game_api ohao_ecs ohao_world_render ohao_reflect)
