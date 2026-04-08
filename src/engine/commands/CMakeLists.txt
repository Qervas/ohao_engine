add_library(ohao_commands STATIC
    command.cpp
    command.hpp
)

target_include_directories(ohao_commands PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
