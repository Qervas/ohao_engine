file(GLOB_RECURSE SCENE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE SCENE_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# We've moved new_scene.cpp content to scene.cpp, so make sure we only have one implementation
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/new_scene.cpp")
    # If both files exist, exclude the old implementation
    list(REMOVE_ITEM SCENE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/new_scene.cpp")
endif()

target_sources(ohao_core PRIVATE ${SCENE_SOURCES} ${SCENE_HEADERS})

target_include_directories(ohao_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}) 