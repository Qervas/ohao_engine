# The cooker (system design 4.8, step 5): source files into cooked assets, cached by content. The one library that
# reads a model's source (the importer: model_loader.cpp, Assimp, stb_image; gltf_structure: a glTF file's structure,
# its nodes, extras, lights, cameras and primitive counts, which the importer and the level module read); hosts link it
# to cook on demand.
add_library(ohao_cook STATIC cook.cpp cook.hpp model.hpp model_loader.cpp model_loader.hpp gltf_structure.cpp
    gltf_structure.hpp)
target_include_directories(ohao_cook PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_cook PUBLIC cxx_std_20)
target_link_libraries(ohao_cook
    PUBLIC ohao_core ohao_asset glm
    PRIVATE ohao_stb_image ohao_simdjson ohao_bc7enc assimp)
target_include_directories(ohao_cook PRIVATE ${assimp_SOURCE_DIR}/include ${assimp_BINARY_DIR}/include)
