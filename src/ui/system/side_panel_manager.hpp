#pragma once

#include "ui/common/panel_base.hpp"
#include "ui/icons/font_awesome_icons.hpp"
#include <imgui.h>
#include <memory>
#include <vector>
#include <functional>
#include <string>

namespace ohao {

// Side panel tab types (extensible)
enum class SidePanelTab {
    Properties = 0,
    SceneSettings,
    RenderSettings,
    Physics,
    // Future tabs can be added here:
    // Materials,
    // AssetBrowser,
    // Animation,
    COUNT
};

// Tab metadata
struct SidePanelTabInfo {
    SidePanelTab tabType;
    const char* icon;          // FontAwesome icon
    const char* tooltip;       // Hover tooltip
    PanelBase* panel;          // Panel to render when active

    SidePanelTabInfo(SidePanelTab type, const char* ic, const char* tip, PanelBase* p)
        : tabType(type), icon(ic), tooltip(tip), panel(p) {}
};

/**
 * Blender-style side panel manager with vertical icon tabs
 * Manages a tabbed panel system with icon bar on the left edge
 */
class SidePanelManager {
public:
    SidePanelManager();
    ~SidePanelManager() = default;

    // Register a panel with a tab
    void registerTab(SidePanelTab tabType, const char* icon, const char* tooltip, PanelBase* panel);

    // Render the side panel system (icon bar + content)
    void render();

    // Set active tab programmatically
    void setActiveTab(SidePanelTab tab);
    SidePanelTab getActiveTab() const { return m_activeTab; }

    // Get tab info
    const SidePanelTabInfo* getTabInfo(SidePanelTab tab) const;

private:
    void renderIconBar();
    void renderContentArea();
    void renderTabButton(const SidePanelTabInfo& tabInfo, bool isActive);

    std::vector<SidePanelTabInfo> m_tabs;
    SidePanelTab m_activeTab;

    // UI constants - sized for clear FontAwesome icon display
    static constexpr float ICON_BAR_WIDTH = 48.0f;
    static constexpr float ICON_BUTTON_SIZE = 40.0f;
    static constexpr float ICON_PADDING = 4.0f;
};

} // namespace ohao
