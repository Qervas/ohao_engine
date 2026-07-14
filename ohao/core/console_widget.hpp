#pragma once

/**
 * Thread-safe logging facade.
 *
 * Art notes:
 *   - string_view messages; optional source_location (C++20) for file:line.
 *   - LogLevel is enum class; to_underlying via concepts.
 *   - Macros pass source_location::current() automatically.
 */

#include "core/concepts.hpp"

#include <functional>
#include <iostream>
#include <mutex>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ohao {

enum class LogLevel : int {
    Info = 0,
    Warning = 1,
    Error = 2,
    Debug = 3,
};

[[nodiscard]] constexpr std::string_view logLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARNING";
        case LogLevel::Error:   return "ERROR";
        case LogLevel::Debug:   return "DEBUG";
    }
    return "INFO";
}

using LogCallback = std::function<void(LogLevel, std::string_view)>;

class ConsoleWidget {
public:
    ConsoleWidget() = default;
    ~ConsoleWidget() = default;

    ConsoleWidget(const ConsoleWidget&) = delete;
    ConsoleWidget& operator=(const ConsoleWidget&) = delete;

    void log(std::string_view message,
             const std::source_location& loc = std::source_location::current());
    void logWarning(std::string_view message,
                    const std::source_location& loc = std::source_location::current());
    void logError(std::string_view message,
                  const std::source_location& loc = std::source_location::current());
    void logDebug(std::string_view message,
                  const std::source_location& loc = std::source_location::current());

    void logAt(LogLevel level,
               std::string_view message,
               const std::source_location& loc = std::source_location::current());

    void setLogCallback(LogCallback callback);
    void clearLogCallback();

    /// When true (default), prefix messages with file:line from source_location.
    void setIncludeLocation(bool enabled) noexcept { m_includeLocation = enabled; }
    [[nodiscard]] bool includeLocation() const noexcept { return m_includeLocation; }

    template<typename T>
    ConsoleWidget& operator<<(const T& value) {
        std::ostringstream ss;
        ss << value;
        log(ss.str());
        return *this;
    }

    [[nodiscard]] static ConsoleWidget& get() {
        static ConsoleWidget instance;
        return instance;
    }

private:
    void emit(LogLevel level, std::string_view message, const std::source_location& loc);

    std::mutex m_mutex;
    LogCallback m_logCallback;
    bool m_includeLocation{false};  // off by default to preserve existing log format
};

// Macros capture call site. Prefer these over direct calls when location helps.
#define OHAO_LOG(msg) \
    ::ohao::ConsoleWidget::get().log((msg), ::std::source_location::current())
#define OHAO_LOG_INFO(msg) \
    ::ohao::ConsoleWidget::get().log((msg), ::std::source_location::current())
#define OHAO_LOG_WARNING(msg) \
    ::ohao::ConsoleWidget::get().logWarning((msg), ::std::source_location::current())
#define OHAO_LOG_ERROR(msg) \
    ::ohao::ConsoleWidget::get().logError((msg), ::std::source_location::current())
#define OHAO_LOG_DEBUG(msg) \
    ::ohao::ConsoleWidget::get().logDebug((msg), ::std::source_location::current())

} // namespace ohao
