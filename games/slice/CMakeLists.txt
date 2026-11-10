# The slice (step 3i): the game the engine is built with, a game::Module (slice/module.hpp). Its sources include the
# engine's headers as "ecs/...", its own as "slice/...".
file(GLOB SLICE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_slice STATIC ${SLICE_SOURCES})
target_include_directories(ohao_slice PUBLIC ${CMAKE_SOURCE_DIR}/games)
target_compile_features(ohao_slice PUBLIC cxx_std_20)
target_link_libraries(ohao_slice PUBLIC ohao_game_api ohao_ecs ohao_world_render ohao_reflect)
