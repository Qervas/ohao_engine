add_subdirectory(core)
add_subdirectory(renderer)
add_subdirectory(utils)
add_subdirectory(ui)
add_subdirectory(tests)

add_executable(ohao_engine
    main.cpp
    tests/multi_object_demo.cpp
)

target_link_libraries(ohao_engine PRIVATE
    ohao_core
    ohao_renderer
    ohao_utils
    ohao_ui
)

add_dependencies(ohao_engine shaders)

target_include_directories(ohao_engine PRIVATE
    ${CMAKE_BINARY_DIR}
)
