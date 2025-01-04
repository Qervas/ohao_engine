add_library(ohao_utils INTERFACE)

# Find all headers and source files recursively
file(GLOB_RECURSE UTILS_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

file(GLOB_RECURSE UTILS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)

if(UTILS_SOURCES)
    add_library(ohao_utils STATIC
        ${UTILS_HEADERS}
        ${UTILS_SOURCES}
    )
endif()

# Set include directories
target_include_directories(ohao_utils
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

source_group(
    TREE ${CMAKE_CURRENT_SOURCE_DIR}
    PREFIX "Header Files"
    FILES ${UTILS_HEADERS}
)

if(UTILS_SOURCES)
    source_group(
        TREE ${CMAKE_CURRENT_SOURCE_DIR}
        PREFIX "Source Files"
        FILES ${UTILS_SOURCES}
    )
endif()
