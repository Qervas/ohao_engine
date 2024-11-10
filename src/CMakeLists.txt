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
