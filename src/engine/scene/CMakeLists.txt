file(GLOB_RECURSE SCENE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/scene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scene_object.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/transform.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/default_scene_factory.cpp"
)
file(GLOB_RECURSE SCENE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/scene.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scene_object.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/transform.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/default_scene_factory.hpp"
)

# We've moved new_scene.cpp content to scene.cpp, so make sure we only have one implementation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/new_scene.cpp")
    # If both files exist, exclude the old implementation
    list(REMOVE_ITEM SCENE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/new_scene.cpp")
endif()

add_library(ohao_scene STATIC ${SCENE_SOURCES} ${SCENE_HEADERS})

target_include_directories(ohao_scene PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/imgui
    ${CMAKE_SOURCE_DIR}/external/imgui/backends
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_scene PUBLIC
    glm
    imgui
    glfw
    nlohmann_json::nlohmann_json
    ohao_component
    ${Vulkan_LIBRARIES}
) 