# ohao_anim (sub-project 10): skeletons, clips, animation graphs and their evaluation, and the glTF
# reader for skins and animations. No ECS and no Vulkan: core, glm, and simdjson privately for JSON.
file(GLOB ANIM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB ANIM_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_anim STATIC ${ANIM_SOURCES} ${ANIM_HEADERS})
target_include_directories(ohao_anim PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_anim PUBLIC cxx_std_20)
target_link_libraries(ohao_anim PUBLIC ohao_core glm PRIVATE ohao_simdjson)
