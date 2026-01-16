#include "console_widget.hpp"

namespace ohao {

void ConsoleWidget::setLogCallback(LogCallback callback) {
    std::lock_guard<std::mutex> lock(mutex);
    logCallback = callback;
}

void ConsoleWidget::clearLogCallback() {
    std::lock_guard<std::mutex> lock(mutex);
    logCallback = nullptr;
}

void ConsoleWidget::log(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (logCallback) {
        logCallback(LogLevel::Info, message);
    } else {
        std::cout << "[INFO] " << message << std::endl;
    }
}

void ConsoleWidget::logWarning(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (logCallback) {
        logCallback(LogLevel::Warning, message);
    } else {
        std::cout << "[WARNING] " << message << std::endl;
    }
}

void ConsoleWidget::logError(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (logCallback) {
        logCallback(LogLevel::Error, message);
    } else {
        std::cerr << "[ERROR] " << message << std::endl;
    }
}

void ConsoleWidget::logDebug(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (logCallback) {
        logCallback(LogLevel::Debug, message);
    } else {
        std::cout << "[DEBUG] " << message << std::endl;
    }
}

}
