#include "console_widget.hpp"
#include <cmath>
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
    if (ImGui::Button("Copy Selected")) {
        copySelectedEntries();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy All")) {
        copyAllEntries();
    }
    ImGui::SameLine();
    if (ImGui::Button("Options"))
        ImGui::OpenPopup("Options");

    ImGui::Separator();

    // Console content
    const float footer_height_to_reserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), false, ImGuiWindowFlags_HorizontalScrollbar);

    std::lock_guard<std::mutex> lock(mutex);
    for (int i = 0; i < entries.size(); i++) {
        const auto& entry = entries[i];

        ImGui::PushID(i);

        // Selectable with subtle highlight
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.35f, 0.55f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.28f, 0.48f, 0.75f, 0.70f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.35f, 0.60f, 0.90f, 0.80f));

        ImGui::Selectable("##line", &entry.selected, ImGuiSelectableFlags_SpanAllColumns);

        ImGui::PopStyleColor(3);

        // Position for the actual text
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetStyle().ItemSpacing.x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetTextLineHeight());

        // Render the actual text with better readability
        if (showTimestamps) {
            ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.70f, 1.0f),  // Lighter gray for better readability
                "[%s] ", entry.timeStr.c_str());
            ImGui::SameLine();
        }
        if (showCategories) {
            ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.70f, 1.0f),  // Lighter gray for better readability
                "[%s] ", entry.category.c_str());
            ImGui::SameLine();
        }
        ImGui::TextColored(entry.color, "%s", entry.message.c_str());

        ImGui::PopID();
    }

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

void ConsoleWidget::log(const std::string& message) {
    addEntry(message, ImVec4(0.90f, 0.90f, 0.92f, 1.0f));  // Slightly off-white for regular messages
}

void ConsoleWidget::logWarning(const std::string& message) {
    addEntry(message, ImVec4(1.00f, 0.75f, 0.20f, 1.0f));  // Orange-yellow for warnings
}

void ConsoleWidget::logError(const std::string& message) {
    addEntry(message, ImVec4(1.00f, 0.35f, 0.35f, 1.0f));  // Softer red for errors (less jarring)
}

void ConsoleWidget::logDebug(const std::string& message) {
    addEntry(message, ImVec4(0.60f, 0.85f, 0.60f, 1.0f), "Debug");  // Softer green for debug
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
    float relativeTime = std::chrono::duration<float>(now - startTime).count();

    // Format the time string when creating the entry
    auto timeT = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    auto timeInfo = std::localtime(&timeT);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", timeInfo);

    std::stringstream ss;
    ss << buffer << "." << std::setfill('0') << std::setw(3)
       << static_cast<int>((relativeTime - std::floor(relativeTime)) * 1000);
    std::string timeStr = ss.str();

    entries.push_back({message, color, relativeTime, category, timeStr});
}

void ConsoleWidget::copySelectedEntries() {
    std::string selectedText;
    for (const auto& entry : entries) {
        if (entry.selected) {
            if (showTimestamps)
                selectedText += "[" + entry.timeStr + "] ";
            if (showCategories)
                selectedText += "[" + entry.category + "] ";
            selectedText += entry.message + "\n";
        }
    }
    if (!selectedText.empty()) {
        ImGui::SetClipboardText(selectedText.c_str());
    }
}

void ConsoleWidget::copyAllEntries() {
    std::string allText;
    for (const auto& entry : entries) {
        if (showTimestamps)
            allText += "[" + entry.timeStr + "] ";
        if (showCategories)
            allText += "[" + entry.category + "] ";
        allText += entry.message + "\n";
    }
    if (!allText.empty()) {
        ImGui::SetClipboardText(allText.c_str());
    }
}

std::string ConsoleWidget::formatTimestamp(float timestamp) {
    std::time_t time = std::time(nullptr);
    std::tm* tm = std::localtime(&time);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", tm);

    std::stringstream ss;
    ss << buffer << "." << std::setfill('0') << std::setw(3)
       << static_cast<int>((timestamp - std::floor(timestamp)) * 1000);
    return ss.str();
}


}
