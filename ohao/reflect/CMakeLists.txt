# ohao_reflect -- type registry, canonical JSON writer, schema export.
# Depends on glm and the standard library only (no EnTT, Vulkan or JSON library), so
# tools and bindings can link it without the renderer.
file(GLOB REFLECT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB REFLECT_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_reflect STATIC ${REFLECT_SOURCES} ${REFLECT_HEADERS})
target_include_directories(ohao_reflect PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_reflect PUBLIC cxx_std_20)
target_link_libraries(ohao_reflect PUBLIC glm)
