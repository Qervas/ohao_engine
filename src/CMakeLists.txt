add_subdirectory(engine)
add_subdirectory(physics)
add_subdirectory(renderer)
add_subdirectory(utils)
add_subdirectory(ui)
add_subdirectory(tests)

add_executable(ohao_app
    main.cpp
    tests/multi_object_demo.cpp
)

target_link_libraries(ohao_app PRIVATE
    ohao_engine
    ohao_physics
    ohao_renderer
    ohao_utils
    ohao_ui
)

add_dependencies(ohao_app shaders)

target_include_directories(ohao_app PRIVATE
    ${CMAKE_BINARY_DIR}
)
