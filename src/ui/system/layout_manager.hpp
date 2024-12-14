#pragma once
#include "imgui.h"
#include <string>

namespace ohao {

class LayoutManager {
public:
    static void initializeLayout(ImGuiID dockspaceId);
    static void resetLayout(ImGuiID dockspaceId);

private:
    static constexpr float VIEWPORT_WIDTH_RATIO = 0.8f;    // 80% of window width
    static constexpr float RIGHT_PANEL_WIDTH_RATIO = 0.2f; // 20% of window width
    static constexpr float CONSOLE_HEIGHT_RATIO = 0.25f;   // 25% of viewport height
    static constexpr float OUTLINER_HEIGHT_RATIO = 0.3f;   // 30% of right panel
    static constexpr float SCENE_SETTINGS_HEIGHT_RATIO = 0.3f; // 30% of right panel

    static void setupDefaultLayout(ImGuiID dockspaceId);
    static void arrangeRightPanels(ImGuiID rightPanelId);
};

} // namespace ohao
