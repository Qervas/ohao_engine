add_library(ohao_events STATIC
    event_bus.cpp
    event_bus.hpp
)

target_include_directories(ohao_events PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
