#include "side_panel_manager.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include "engine/actor/actor.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "physics/components/physics_component.hpp"
#include "renderer/components/light_component.hpp"

namespace ohao {

SidePanelManager::SidePanelManager()
    : m_activeTab(SidePanelTab::Properties)
    , m_activeComponentTab(ComponentTab::Mesh)
    , m_componentTabActive(false) {
    m_tabs.reserve(static_cast<size_t>(SidePanelTab::COUNT));
    m_componentTabs.reserve(static_cast<size_t>(ComponentTab::COUNT));
}

void SidePanelManager::registerTab(SidePanelTab tabType, const char* icon,
                                   const char* tooltip, PanelBase* panel) {
    m_tabs.emplace_back(tabType, icon, tooltip, panel);
}

void SidePanelManager::registerComponentTab(ComponentTab tabType, const char* icon,
                                            const char* tooltip, PanelBase* panel) {
    m_componentTabs.emplace_back(tabType, icon, tooltip, panel);
}

void SidePanelManager::setActiveTab(SidePanelTab tab) {
    m_activeTab = tab;
    m_componentTabActive = false;
}

void SidePanelManager::setActiveComponentTab(ComponentTab tab) {
    m_activeComponentTab = tab;
    m_componentTabActive = true;
}

const SidePanelTabInfo* SidePanelManager::getTabInfo(SidePanelTab tab) const {
    for (const auto& tabInfo : m_tabs) {
        if (tabInfo.tabType == tab) {
            return &tabInfo;
        }
    }
    return nullptr;
}

const ComponentTabInfo* SidePanelManager::getComponentTabInfo(ComponentTab tab) const {
    for (const auto& tabInfo : m_componentTabs) {
        if (tabInfo.tabType == tab) {
            return &tabInfo;
        }
    }
    return nullptr;
}

void SidePanelManager::render() {
    // Get the available region for the side panel
    ImVec2 availableRegion = ImGui::GetContentRegionAvail();
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();

    // Render icon bar on the left
    renderIconBar();

    // Render content area on the right
    ImGui::SameLine(0.0f, 0.0f);  // No spacing between icon bar and content
    renderContentArea();
}

void SidePanelManager::renderIconBar() {
    // Create a child window for the icon bar
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ICON_PADDING, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 8.0f));  // More vertical space between icons
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.08f, 1.00f));

    ImVec2 iconBarSize(ICON_BAR_WIDTH, -1);  // Full height

    if (ImGui::BeginChild("##SidePanelIconBar", iconBarSize, true,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {

        // Render each scene-level tab button
        for (const auto& tabInfo : m_tabs) {
            bool isActive = (!m_componentTabActive && tabInfo.tabType == m_activeTab);
            renderTabButton(tabInfo, isActive);
        }

        // Check if any component tabs are visible
        bool hasVisibleComponentTabs = false;
        for (const auto& tabInfo : m_componentTabs) {
            if (tabInfo.visible) {
                hasVisibleComponentTabs = true;
                break;
            }
        }

        // Render separator if there are visible component tabs
        if (hasVisibleComponentTabs) {
            renderSeparator();

            // Render component tab buttons
            for (const auto& tabInfo : m_componentTabs) {
                if (tabInfo.visible) {
                    bool isActive = (m_componentTabActive && tabInfo.tabType == m_activeComponentTab);
                    renderComponentTabButton(tabInfo, isActive);
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void SidePanelManager::renderTabButton(const SidePanelTabInfo& tabInfo, bool isActive) {
    // Button colors based on active state
    ImVec4 buttonColor, hoverColor, activeColor;

    if (isActive) {
        // Active tab - blue highlight
        buttonColor = ImVec4(0.28f, 0.65f, 0.95f, 1.00f);
        hoverColor  = ImVec4(0.35f, 0.70f, 1.00f, 1.00f);
        activeColor = ImVec4(0.25f, 0.60f, 0.90f, 1.00f);
    } else {
        // Inactive tab - dark gray
        buttonColor = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
        hoverColor  = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
        activeColor = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    }

    ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeColor);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

    // Render the icon button
    ImVec2 buttonSize(ICON_BUTTON_SIZE, ICON_BUTTON_SIZE);
    if (ImGui::Button(tabInfo.icon, buttonSize)) {
        setActiveTab(tabInfo.tabType);
    }

    // Tooltip on hover
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tabInfo.tooltip);
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

void SidePanelManager::renderContentArea() {
    // Find the active panel (either scene tab or component tab)
    PanelBase* activePanel = nullptr;

    if (m_componentTabActive) {
        // Component tab is active
        const ComponentTabInfo* activeTabInfo = getComponentTabInfo(m_activeComponentTab);
        if (activeTabInfo && activeTabInfo->panel) {
            activePanel = activeTabInfo->panel;
        }
    } else {
        // Scene tab is active
        const SidePanelTabInfo* activeTabInfo = getTabInfo(m_activeTab);
        if (activeTabInfo && activeTabInfo->panel) {
            activePanel = activeTabInfo->panel;
        }
    }

    if (!activePanel) {
        ImGui::TextDisabled("No panel active");
        return;
    }

    // Get available region for content
    ImVec2 contentSize = ImGui::GetContentRegionAvail();

    // Create content area with subtle styling
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.09f, 0.10f, 1.00f));

    if (ImGui::BeginChild("##SidePanelContent", contentSize, false, ImGuiWindowFlags_None)) {
        // Render the active panel's content directly (without its own window)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        // Small header with panel name
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.77f, 1.0f));
        ImGui::TextUnformatted(activePanel->getName().c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        // Render panel content in a scrollable child
        if (ImGui::BeginChild("##PanelContentScroll", ImVec2(0, 0), false)) {
            // Call the panel's render method
            // Note: The panel will call Begin/End internally, which is not ideal
            // We may need to refactor panels to have a renderContent() method
            activePanel->render();
        }
        ImGui::EndChild();

        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void SidePanelManager::renderComponentTabButton(const ComponentTabInfo& tabInfo, bool isActive) {
    // Button colors based on active state (amber/orange theme for components)
    ImVec4 buttonColor, hoverColor, activeColor;

    if (isActive) {
        // Active component tab - amber highlight
        buttonColor = ImVec4(0.85f, 0.55f, 0.20f, 1.00f);
        hoverColor  = ImVec4(0.95f, 0.65f, 0.30f, 1.00f);
        activeColor = ImVec4(0.75f, 0.50f, 0.15f, 1.00f);
    } else {
        // Inactive component tab - warm dark
        buttonColor = ImVec4(0.15f, 0.12f, 0.08f, 1.00f);
        hoverColor  = ImVec4(0.22f, 0.18f, 0.12f, 1.00f);
        activeColor = ImVec4(0.13f, 0.10f, 0.06f, 1.00f);
    }

    ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeColor);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

    // Render the icon button
    ImVec2 buttonSize(ICON_BUTTON_SIZE, ICON_BUTTON_SIZE);
    if (ImGui::Button(tabInfo.icon, buttonSize)) {
        setActiveComponentTab(tabInfo.tabType);
    }

    // Tooltip on hover
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tabInfo.tooltip);
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

void SidePanelManager::renderSeparator() {
    // Render a horizontal line separator between scene and component tabs
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Calculate separator position (centered in icon bar)
    float separatorX = cursorPos.x + (ICON_BAR_WIDTH - SEPARATOR_WIDTH) * 0.5f;
    float separatorY = cursorPos.y + 4.0f;  // Small vertical padding

    // Draw the separator line
    ImVec2 p1(separatorX, separatorY);
    ImVec2 p2(separatorX + SEPARATOR_WIDTH, separatorY + SEPARATOR_HEIGHT);
    ImU32 separatorColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.3f, 0.3f, 0.32f, 1.0f));
    drawList->AddRectFilled(p1, p2, separatorColor);

    // Advance cursor to account for separator height
    ImGui::Dummy(ImVec2(ICON_BAR_WIDTH, SEPARATOR_HEIGHT + 8.0f));  // 8px total margin
}

void SidePanelManager::updateDynamicTabs(Actor* selectedActor) {
    // Hide all component tabs by default
    for (auto& tabInfo : m_componentTabs) {
        tabInfo.visible = false;
    }

    if (!selectedActor) {
        return;
    }

    // Show component tabs based on which components the actor has
    for (auto& tabInfo : m_componentTabs) {
        bool hasComponent = false;

        switch (tabInfo.tabType) {
            case ComponentTab::Mesh:
                hasComponent = (selectedActor->getComponent<MeshComponent>() != nullptr);
                break;
            case ComponentTab::Material:
                hasComponent = (selectedActor->getComponent<MaterialComponent>() != nullptr);
                break;
            case ComponentTab::Physics:
                hasComponent = (selectedActor->getComponent<PhysicsComponent>() != nullptr);
                break;
            case ComponentTab::Light:
                hasComponent = (selectedActor->getComponent<LightComponent>() != nullptr);
                break;
            default:
                break;
        }

        tabInfo.visible = hasComponent;
    }

    // If the current active component tab is no longer visible, switch to a scene tab
    if (m_componentTabActive) {
        const ComponentTabInfo* activeTabInfo = getComponentTabInfo(m_activeComponentTab);
        if (!activeTabInfo || !activeTabInfo->visible) {
            // Switch back to Properties tab
            setActiveTab(SidePanelTab::Properties);
        }
    }
}

} // namespace ohao
