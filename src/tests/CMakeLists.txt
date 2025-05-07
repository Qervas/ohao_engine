file(GLOB_RECURSE TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE TEST_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# Create a list of included files for display purposes
set(INCLUDED_TEST_FILES
    ${TEST_SOURCES}
    ${TEST_HEADERS}
)

# Log included files
foreach(file ${INCLUDED_TEST_FILES})
    message(STATUS "Including test file: ${file}")
endforeach()

# Add include directories
target_include_directories(ohao_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}) 