# Find all source files in the UI directory
file(GLOB_RECURSE UI_SOURCES 
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# Make sure we glob all headers for IDE integration
file(GLOB_RECURSE UI_HEADERS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

# Add required imgui backend files
set(EXTERNAL_SOURCES
  "${CMAKE_SOURCE_DIR}/external/imgui/backends/imgui_impl_glfw.cpp"
  "${CMAKE_SOURCE_DIR}/external/imgui/backends/imgui_impl_vulkan.cpp"
)

message(STATUS "UI Sources: ${UI_SOURCES}")
message(STATUS "External Sources: ${EXTERNAL_SOURCES}")

# Create library with all sources and headers
add_library(ohao_ui ${UI_SOURCES} ${UI_HEADERS} ${EXTERNAL_SOURCES})

# Automatically find all subdirectories for includes
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
    ohao_core
    ohao_renderer
    ohao_utils
)

# Group UI sources in the IDE
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX "UI" FILES ${UI_SOURCES} ${UI_HEADERS})

# Group external sources in the IDE
source_group("External/ImGui" FILES ${EXTERNAL_SOURCES})
