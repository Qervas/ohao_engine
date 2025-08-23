file(GLOB_RECURSE UI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
message(STATUS "UI Sources: ${UI_SOURCES}")
add_library(ohao_ui ${UI_SOURCES})

file(GLOB UI_DIRS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/*")
set(UI_INCLUDE_DIRS "")
foreach(DIR ${UI_DIRS})
    if(IS_DIRECTORY ${DIR})
        list(APPEND UI_INCLUDE_DIRS ${DIR})
    endif()
endforeach()

# Include directories
target_include_directories(ohao_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${UI_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/imgui
    ${CMAKE_SOURCE_DIR}/external/imgui/backends
    ${CMAKE_BUILD_DIR}/_deps/tinyfieldialogs-src
    ${CMAKE_BUILD_DIR}/_deps/json-src/include
    ${Vulkan_INCLUDE_DIRS}
)

# Link dependencies
target_link_libraries(ohao_ui PUBLIC
    imgui
    imgui_stdlib
    tinyfiledialogs
    nlohmann_json::nlohmann_json
    ${Vulkan_LIBRARIES}
    glfw
    ohao_engine
    ohao_physics
    ohao_renderer
    ohao_utils
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${UI_SOURCES})
