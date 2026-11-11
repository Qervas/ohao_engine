# The cooker (system design 4.8, step 5): source files into cooked assets, cached by content. The one library that
# reads a model's source; hosts link it to cook on demand. The importers (ModelLoader, Assimp) join it from ohao_asset
# when the runtime reads cooked data only (5c).
add_library(ohao_cook STATIC cook.cpp cook.hpp)
target_include_directories(ohao_cook PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_cook PUBLIC cxx_std_20)
target_link_libraries(ohao_cook PUBLIC ohao_core ohao_asset glm PRIVATE ohao_simdjson)
