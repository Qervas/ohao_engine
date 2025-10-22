#include "side_panel_manager.hpp"
#include <imgui.h>
#include <imgui_internal.h>

namespace ohao {

SidePanelManager::SidePanelManager()
    : m_activeTab(SidePanelTab::Properties) {
    m_tabs.reserve(static_cast<size_t>(SidePanelTab::COUNT));
}

void SidePanelManager::registerTab(SidePanelTab tabType, const char* icon,
                                   const char* tooltip, PanelBase* panel) {
    m_tabs.emplace_back(tabType, icon, tooltip, panel);
}

void SidePanelManager::setActiveTab(SidePanelTab tab) {
    m_activeTab = tab;
}

const SidePanelTabInfo* SidePanelManager::getTabInfo(SidePanelTab tab) const {
    for (const auto& tabInfo : m_tabs) {
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

        // Render each tab button
        for (const auto& tabInfo : m_tabs) {
            bool isActive = (tabInfo.tabType == m_activeTab);
            renderTabButton(tabInfo, isActive);
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
    // Find the active panel
    const SidePanelTabInfo* activeTabInfo = getTabInfo(m_activeTab);

    if (!activeTabInfo || !activeTabInfo->panel) {
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
        PanelBase* activePanel = activeTabInfo->panel;

        // We need to render the panel content without calling Begin/End
        // For now, we'll create a temporary window
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

} // namespace ohao
