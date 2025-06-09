file(GLOB_RECURSE SERIALIZATION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

target_sources(ohao_core PRIVATE ${SERIALIZATION_SOURCES})
target_include_directories(ohao_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}) 