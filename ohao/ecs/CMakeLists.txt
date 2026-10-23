file(GLOB_RECURSE ECS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ECS_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_ecs STATIC ${ECS_SOURCES} ${ECS_HEADERS})
target_include_directories(ohao_ecs PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_ecs PUBLIC cxx_std_20)
target_link_libraries(ohao_ecs PUBLIC ohao_core ohao_renderer ohao_scene ohao_reflect glm EnTT::EnTT
    ohao_unordered_dense)
target_link_libraries(ohao_ecs PRIVATE ohao_simdjson)
