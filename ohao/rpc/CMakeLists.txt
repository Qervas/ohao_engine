# ohao_rpc: JSON-RPC 2.0 over newline-delimited streams and the MCP adapter (sub-project 4).
# CPU-only: it depends on ohao_api and ohao_core and never includes Vulkan, GLFW or the
# renderer (reflect_tests enforces it); a host hands it a Session.
file(GLOB RPC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB RPC_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
add_library(ohao_rpc STATIC ${RPC_SOURCES} ${RPC_HEADERS})
target_include_directories(ohao_rpc PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_rpc PUBLIC cxx_std_20)
target_link_libraries(ohao_rpc PUBLIC ohao_api ohao_core)
