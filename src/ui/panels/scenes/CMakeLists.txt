set(SCENES_PANEL_SOURCES
    scenes_panel.cpp
)

set(SCENES_PANEL_HEADERS
    scenes_panel.hpp
)

target_sources(ohao_ui
    PRIVATE
    ${SCENES_PANEL_SOURCES}
    ${SCENES_PANEL_HEADERS}
) 