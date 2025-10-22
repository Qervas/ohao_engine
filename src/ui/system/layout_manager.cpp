#include "layout_manager.hpp"
#include "imgui_internal.h"

namespace ohao {

void LayoutManager::initializeLayout(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    setupDefaultLayout(dockspaceId);
}

void LayoutManager::resetLayout(ImGuiID dockspaceId) {
    initializeLayout(dockspaceId);
}

void LayoutManager::setupDefaultLayout(ImGuiID dockspaceId) {
    // Split into main area and right panel
    auto dock_main_id = dockspaceId;
    auto dock_right_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Right,
                                                    RIGHT_PANEL_WIDTH_RATIO,
                                                    nullptr, &dock_main_id);

    // Split main area to create console at bottom
    auto dock_console_id = ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down,
                                                      CONSOLE_HEIGHT_RATIO,
                                                      nullptr, &dock_main_id);

    // Arrange right side panels
    arrangeRightPanels(dock_right_id);

    // Dock windows
    ImGui::DockBuilderDockWindow("Scene Viewport", dock_main_id);
    ImGui::DockBuilderDockWindow("Console", dock_console_id);

    ImGui::DockBuilderFinish(dockspaceId);
}

void LayoutManager::arrangeRightPanels(ImGuiID rightPanelId) {
    // Split right panel into Outliner (top) and Side Panel (bottom)
    auto remaining_right_id = rightPanelId;
    auto dock_outliner_id = ImGui::DockBuilderSplitNode(remaining_right_id, ImGuiDir_Up,
                                                       OUTLINER_HEIGHT_RATIO,
                                                       nullptr, &remaining_right_id);

    // The remaining space is for the Side Panel (tabbed interface)
    auto dock_side_panel_id = remaining_right_id;

    // Dock the windows
    ImGui::DockBuilderDockWindow("Outliner", dock_outliner_id);
    ImGui::DockBuilderDockWindow("Side Panel", dock_side_panel_id);
}

} // namespace ohao
