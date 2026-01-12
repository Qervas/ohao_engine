add_subdirectory(actor)
add_subdirectory(asset)
add_subdirectory(component)
add_subdirectory(input)
add_subdirectory(scene)
add_subdirectory(serialization)

add_library(ohao_engine INTERFACE)

target_link_libraries(ohao_engine INTERFACE
    ohao_actor
    ohao_asset
    ohao_component
    ohao_input
    ohao_scene
    ohao_serialization
)

target_include_directories(ohao_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)