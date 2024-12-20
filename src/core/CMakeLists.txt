file(GLOB_RECURSE CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/scene/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scene/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/material/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/material/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/asset/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/asset/*.hpp"
)

add_library(ohao_core ${CORE_SOURCES})

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
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(ohao_core PUBLIC
    ${Vulkan_LIBRARIES}
    glfw
    imgui
    imgui_stdlib
    ohao_utils
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CORE_SOURCES})
