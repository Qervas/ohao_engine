#pragma once
#include <sstream>
#include <string>
#include <mutex>
#include <iostream>
#include <functional>

namespace ohao {

// Log level for callback
enum class LogLevel {
    Info,
    Warning,
    Error,
    Debug
};

// Log callback type: (level, message)
using LogCallback = std::function<void(LogLevel, const std::string&)>;

class ConsoleWidget {
public:
    ConsoleWidget() = default;
    ~ConsoleWidget() = default;

    // Thread-safe logging methods
    void log(const std::string& message);
    void logWarning(const std::string& message);
    void logError(const std::string& message);
    void logDebug(const std::string& message);

    // Set external log callback (e.g., for Godot integration)
    // When set, logs are forwarded to the callback instead of stdout
    void setLogCallback(LogCallback callback);
    void clearLogCallback();

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
    std::mutex mutex;
    LogCallback logCallback;
};

// Global logging functions
#define OHAO_LOG(msg) ohao::ConsoleWidget::get().log(msg)
#define OHAO_LOG_INFO(msg) ohao::ConsoleWidget::get().log(msg)
#define OHAO_LOG_WARNING(msg) ohao::ConsoleWidget::get().logWarning(msg)
#define OHAO_LOG_ERROR(msg) ohao::ConsoleWidget::get().logError(msg)
#define OHAO_LOG_DEBUG(msg) ohao::ConsoleWidget::get().logDebug(msg)
}
