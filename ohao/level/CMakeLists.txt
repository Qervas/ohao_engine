# The level pipeline (sub-project 5), in two libraries:
#
# ohao_level -- a glTF file's structure (nodes, extras, lights, cameras, primitive counts) read
# from its JSON. ohao_asset links it (ModelLoader's per-mesh loading needs the primitive
# counts), so it depends on nothing above core: glm, and simdjson privately for the JSON.
add_library(ohao_level STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/gltf_structure.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gltf_structure.hpp)
target_include_directories(ohao_level PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_level PUBLIC cxx_std_20)
target_link_libraries(ohao_level PUBLIC ohao_core glm PRIVATE ohao_simdjson)

# ohao_level_world -- levels in the world: node extras -> hints (level_hints) and the `level`
# sync system that expands a Level component into entities (level_system). It sits above the
# ECS (components, the scene reader for passed-through components); hosts install it with
# level::installLevels.
add_library(ohao_level_world STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/level_hints.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/level_hints.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/level_system.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/level_system.hpp)
target_include_directories(ohao_level_world PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_level_world PUBLIC cxx_std_20)
target_link_libraries(ohao_level_world PUBLIC ohao_level ohao_ecs ohao_reflect ohao_unordered_dense)
