# ohao_ui - minimal logging library
set(UI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/components/console_widget.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/console_widget.hpp"
)

add_library(ohao_ui ${UI_SOURCES})

target_include_directories(ohao_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${UI_SOURCES})
