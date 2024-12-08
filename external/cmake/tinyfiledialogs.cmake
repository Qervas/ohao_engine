include(FetchContent)

FetchContent_Declare(
    tinyfiledialogs
    GIT_REPOSITORY https://git.code.sf.net/p/tinyfiledialogs/code
    GIT_TAG master
)

FetchContent_MakeAvailable(tinyfiledialogs)

# Create library target for tinyfiledialogs
add_library(tinyfiledialogs STATIC
    ${tinyfiledialogs_SOURCE_DIR}/tinyfiledialogs.c
)

target_include_directories(tinyfiledialogs PUBLIC
    ${tinyfiledialogs_SOURCE_DIR}
)

# On Unix-like systems, need to link against X11
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(tinyfiledialogs PUBLIC ${X11_LIBRARIES})
endif()