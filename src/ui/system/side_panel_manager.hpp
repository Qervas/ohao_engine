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

// Component-specific tabs (dynamic, shown based on selection)
enum class ComponentTab {
    Mesh = 0,
    Material,
    Physics,
    Light,
    COUNT
};

// Tab metadata
struct SidePanelTabInfo {
    SidePanelTab tabType;
    const char* icon;          // FontAwesome icon
    const char* tooltip;       // Hover tooltip
    PanelBase* panel;          // Panel to render when active
    bool isDynamic{false};     // Is this a dynamic component tab?

    SidePanelTabInfo(SidePanelTab type, const char* ic, const char* tip, PanelBase* p, bool dynamic = false)
        : tabType(type), icon(ic), tooltip(tip), panel(p), isDynamic(dynamic) {}
};

// Component tab metadata
struct ComponentTabInfo {
    ComponentTab tabType;
    const char* icon;          // FontAwesome icon
    const char* tooltip;       // Hover tooltip
    PanelBase* panel;          // Panel to render when active
    bool visible{false};       // Is this tab currently visible?

    ComponentTabInfo(ComponentTab type, const char* ic, const char* tip, PanelBase* p)
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

    // Register a component panel with a dynamic tab
    void registerComponentTab(ComponentTab tabType, const char* icon, const char* tooltip, PanelBase* panel);

    // Render the side panel system (icon bar + content)
    void render();

    // Set active tab programmatically
    void setActiveTab(SidePanelTab tab);
    void setActiveComponentTab(ComponentTab tab);

    SidePanelTab getActiveTab() const { return m_activeTab; }
    ComponentTab getActiveComponentTab() const { return m_activeComponentTab; }
    bool isComponentTabActive() const { return m_componentTabActive; }

    // Get tab info
    const SidePanelTabInfo* getTabInfo(SidePanelTab tab) const;
    const ComponentTabInfo* getComponentTabInfo(ComponentTab tab) const;

    // Update which component tabs are visible based on selected actor
    void updateDynamicTabs(class Actor* selectedActor);

private:
    void renderIconBar();
    void renderContentArea();
    void renderTabButton(const SidePanelTabInfo& tabInfo, bool isActive);
    void renderComponentTabButton(const ComponentTabInfo& tabInfo, bool isActive);
    void renderSeparator();

    std::vector<SidePanelTabInfo> m_tabs;
    std::vector<ComponentTabInfo> m_componentTabs;

    SidePanelTab m_activeTab;
    ComponentTab m_activeComponentTab;
    bool m_componentTabActive{false};  // Is a component tab currently active?

    // UI constants - sized for clear FontAwesome icon display
    static constexpr float ICON_BAR_WIDTH = 48.0f;
    static constexpr float ICON_BUTTON_SIZE = 40.0f;
    static constexpr float ICON_PADDING = 4.0f;
    static constexpr float SEPARATOR_WIDTH = 32.0f;
    static constexpr float SEPARATOR_HEIGHT = 2.0f;
};

} // namespace ohao
