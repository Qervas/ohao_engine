#pragma once
#include <sstream>
#include <vector>
#include <string>
#include <mutex>
#include <imgui.h>

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
    void logDebug(const std::string& message);
    void clear();

    // Stream-like logging
    template<typename T>
    ConsoleWidget& operator<<(const T& value) {
        std::stringstream ss;
        ss << value;
        log(ss.str());
        return *this;
    }

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
        std::string category;
    };

    std::vector<LogEntry> entries;
    bool autoScroll{true};
    bool showTimestamps{true};
    bool showCategories{true};
    std::mutex mutex;  // For thread-safe logging

    void addEntry(const std::string& message, const ImVec4& color, const std::string& category = "Info");
};

// Global logging functions
#define OHAO_LOG(msg) ohao::ConsoleWidget::get().log(msg)
#define OHAO_LOG_WARNING(msg) ohao::ConsoleWidget::get().logWarning(msg)
#define OHAO_LOG_ERROR(msg) ohao::ConsoleWidget::get().logError(msg)
#define OHAO_LOG_DEBUG(msg) ohao::ConsoleWidget::get().logDebug(msg)
}
