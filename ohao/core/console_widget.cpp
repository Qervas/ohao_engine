#include "console_widget.hpp"

#include <ostream>

namespace ohao {
namespace {

std::ostream& stream_for(LogLevel level) {
    return level == LogLevel::Error ? std::cerr : std::cout;
}

} // namespace

void ConsoleWidget::setLogCallback(LogCallback callback) {
    std::lock_guard lock(m_mutex);
    m_logCallback = std::move(callback);
}

void ConsoleWidget::clearLogCallback() {
    std::lock_guard lock(m_mutex);
    m_logCallback = nullptr;
}

void ConsoleWidget::log(std::string_view message, const std::source_location& loc) {
    emit(LogLevel::Info, message, loc);
}

void ConsoleWidget::logWarning(std::string_view message, const std::source_location& loc) {
    emit(LogLevel::Warning, message, loc);
}

void ConsoleWidget::logError(std::string_view message, const std::source_location& loc) {
    emit(LogLevel::Error, message, loc);
}

void ConsoleWidget::logDebug(std::string_view message, const std::source_location& loc) {
    emit(LogLevel::Debug, message, loc);
}

void ConsoleWidget::logAt(LogLevel level, std::string_view message, const std::source_location& loc) {
    emit(level, message, loc);
}

void ConsoleWidget::emit(LogLevel level, std::string_view message, const std::source_location& loc) {
    std::lock_guard lock(m_mutex);

    if (m_logCallback) {
        m_logCallback(level, message);
        return;
    }

    auto& os = stream_for(level);
    os << '[' << logLevelName(level) << "] ";
    if (m_includeLocation) {
        os << loc.file_name() << ':' << loc.line() << ": ";
    }
    os << message << '\n';
}

} // namespace ohao
