# What the engine loads: the cooked formats (cooked.hpp), the vertex, the content hash, the primitives made in code.
# No source format is read here: the importers are the cooker's (ohao/cook).
file(GLOB_RECURSE ASSET_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
file(GLOB_RECURSE ASSET_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(ohao_asset STATIC ${ASSET_HEADERS} ${ASSET_SOURCES})
target_include_directories(ohao_asset PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_asset PUBLIC cxx_std_20)
target_link_libraries(ohao_asset PUBLIC ohao_core glm)
