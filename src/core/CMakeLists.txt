file(GLOB_RECURSE CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

file(GLOB_RECURSE CORE_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.inl"
)

add_library(ohao_core ${CORE_SOURCES} ${CORE_HEADERS})

target_include_directories(ohao_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

# Set properties for .inl files
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/*.inl
    PROPERTIES
    HEADER_FILE_ONLY TRUE
)

# Get all subdirectories for includes
file(GLOB CORE_DIRS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/*")
set(CORE_INCLUDE_DIRS "")
foreach(DIR ${CORE_DIRS})
    if(IS_DIRECTORY ${DIR})
        list(APPEND CORE_INCLUDE_DIRS ${DIR})
    endif()
endforeach()



target_include_directories(ohao_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/imgui
    ${CMAKE_SOURCE_DIR}/external/imgui/backends
    ${CMAKE_BUILD_DIR}/_deps/json-src/include
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_core PUBLIC
    ${Vulkan_LIBRARIES}
    glfw
    imgui
    imgui_stdlib
    nlohmann_json::nlohmann_json
    ohao_utils
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CORE_SOURCES})

# Add subdirectories for each subsystem
add_subdirectory(asset)
add_subdirectory(material)
add_subdirectory(scene)
add_subdirectory(component)
add_subdirectory(actor)
# Remove ECS system
# add_subdirectory(ecs)
# add_subdirectory(physics)
add_subdirectory(serialization)

# Target include paths
target_include_directories(ohao_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
