set(VIEWPORT_SOURCES
    scene_viewport.cpp
)

set(VIEWPORT_HEADERS
    scene_viewport.hpp
)

target_sources(ohao_ui
    PRIVATE
    ${VIEWPORT_SOURCES}
    ${VIEWPORT_HEADERS}
) 