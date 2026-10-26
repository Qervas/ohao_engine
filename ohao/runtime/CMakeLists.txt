file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE RUNTIME_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_runtime STATIC ${RUNTIME_SOURCES} ${RUNTIME_HEADERS})
target_include_directories(ohao_runtime PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_runtime PUBLIC cxx_std_20)
# ohao_api: the live host serves a Session; capture results are the API's. ohao_audio: footsteps.
target_link_libraries(ohao_runtime PUBLIC ohao_api ohao_audio ohao_core ohao_ecs ohao_level_world ohao_gpu_vulkan glm glfw ${Vulkan_LIBRARIES})
target_include_directories(ohao_runtime PUBLIC ${Vulkan_INCLUDE_DIRS} PRIVATE ${stb_SOURCE_DIR})
