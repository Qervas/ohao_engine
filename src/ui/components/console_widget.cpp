#include "console_widget.hpp"
#include <imgui.h>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace ohao {
ConsoleWidget::ConsoleWidget() {
    clear();
    log("Console initialized");
}

void ConsoleWidget::render() {

    ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Console")) {
        ImGui::End();
        return;
    }

    // Options menu
    if (ImGui::BeginPopup("Options")) {
        ImGui::Checkbox("Auto-scroll", &autoScroll);
        ImGui::Checkbox("Show timestamps", &showTimestamps);
        ImGui::Checkbox("Show categories", &showCategories);
        ImGui::EndPopup();
    }

    // Buttons
    if (ImGui::Button("Clear")) clear();
    ImGui::SameLine();
    if (ImGui::Button("Options"))
        ImGui::OpenPopup("Options");

    ImGui::Separator();

    // Console content
    const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), false, ImGuiWindowFlags_HorizontalScrollbar);

    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : entries) {
        if (showTimestamps) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                "[%.2f] ", entry.timestamp);
            ImGui::SameLine();
        }
        if (showCategories) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                "[%s] ", entry.category.c_str());
            ImGui::SameLine();
        }
        ImGui::TextColored(entry.color, "%s", entry.message.c_str());
    }

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

void ConsoleWidget::log(const std::string& message) {
    addEntry(message, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
}

void ConsoleWidget::logWarning(const std::string& message) {
    addEntry(message, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
}

void ConsoleWidget::logError(const std::string& message) {
    addEntry(message, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
}

void ConsoleWidget::logDebug(const std::string& message) {
    addEntry(message, ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Debug");
}

void ConsoleWidget::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
}

void ConsoleWidget::addEntry(const std::string& message, const ImVec4& color,
                            const std::string& category) {
    std::lock_guard<std::mutex> lock(mutex);

    static auto startTime = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    float timestamp = std::chrono::duration<float>(now - startTime).count();

    entries.push_back({message, color, timestamp, category});
}


}
