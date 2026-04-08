add_subdirectory(actor)
add_subdirectory(asset)
add_subdirectory(commands)
add_subdirectory(component)
add_subdirectory(events)
add_subdirectory(scene)

add_library(ohao_engine INTERFACE)

target_link_libraries(ohao_engine INTERFACE
    ohao_actor
    ohao_asset
    ohao_commands
    ohao_component
    ohao_events
    ohao_scene
)

target_include_directories(ohao_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)