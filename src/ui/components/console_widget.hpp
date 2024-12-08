#pragma once
#include <vector>
#include <string>
#include <mutex>
#include "imgui.h"

namespace ohao {
class ConsoleWidget {
public:
    ConsoleWidget();
    ~ConsoleWidget() = default;

    void render();

    // Thread-safe logging methods
    void log(const std::string& message);
    void logWarning(const std::string& message);
    void logError(const std::string& message);
    void clear();

    // Singleton access
    static ConsoleWidget& get() {
        static ConsoleWidget instance;
        return instance;
    }

private:
    struct LogEntry {
        std::string message;
        ImVec4 color;
        float timestamp;
    };

    std::vector<LogEntry> entries;
    bool autoScroll{true};
    bool showTimestamps{true};
    std::mutex mutex;  // For thread-safe logging

    void addEntry(const std::string& message, const ImVec4& color);
};

// Global logging functions
#define OHAO_LOG(msg) ohao::ConsoleWidget::get().log(msg)
#define OHAO_LOG_WARNING(msg) ohao::ConsoleWidget::get().logWarning(msg)
#define OHAO_LOG_ERROR(msg) ohao::ConsoleWidget::get().logError(msg)
}
