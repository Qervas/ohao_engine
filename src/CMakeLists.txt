add_subdirectory(core)
add_subdirectory(renderer)
add_subdirectory(utils)

add_executable(ohao_engine
    main.cpp
)

target_link_libraries(ohao_engine PRIVATE
    ohao_core
    ohao_renderer
    ohao_utils
)

add_dependencies(ohao_engine shaders)

target_include_directories(ohao_engine PRIVATE
    ${CMAKE_BINARY_DIR}
)
