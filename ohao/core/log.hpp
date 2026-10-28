#pragma once

// The engine's one logger. Every record goes into a 4,096-record ring (always on), then to
// the sinks whose level it passes: stderr (human format, installed by default) and, in
// `ohao serve`, the RPC sink. A call below the capture threshold costs one relaxed load and
// a compare; nothing is formatted.
//
//   OHAO_LOG(Info, render, "deferred renderer ready ({}x{})", w, h);
//
// Levels: Trace, Debug, Info, Warn, Error. Channels: render, gpu, validation, ecs, asset,
// physics, audio, api, rpc, runtime, vfx. The format string is std::format's, checked at compile
// time. Messages are cut at 1 KB. Identical consecutive records collapse into one with a
// repeat count. OHAO_LOG_LEVEL=trace|debug|info|warn|error|off sets the threshold at start.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::log {

enum class Level : uint8_t { Trace, Debug, Info, Warn, Error, Off };

enum class Channel : uint8_t {
    render, gpu, validation, ecs, asset, physics, audio, api, rpc, runtime, vfx, Count
};

inline constexpr std::size_t kRingCapacity = 4096;
inline constexpr std::size_t kMaxMessage = 1024;   // bytes; longer messages are cut

[[nodiscard]] std::string_view levelName(Level level) noexcept;      // "trace" .. "error", "off"
[[nodiscard]] std::string_view channelName(Channel channel) noexcept; // "render" ..
[[nodiscard]] std::optional<Level> parseLevel(std::string_view name) noexcept;
[[nodiscard]] std::optional<Channel> parseChannel(std::string_view name) noexcept;

struct Record {
    uint64_t seq = 0;       // 1, 2, 3 ... one per record (a repeat does not take a new one)
    double t = 0.0;         // seconds since the logger started
    uint64_t frame = 0;     // the frame set by setFrame when the record was written
    Level level = Level::Info;
    Channel channel = Channel::render;
    uint32_t repeat = 1;    // identical consecutive records collapsed into this one
    std::string msg;
};

// A destination for records. write() runs for each new record at or above `level`;
// repeated() when the newest record's repeat count grows. Both run under the logger's lock,
// in record order, so a sink needs no locking of its own for ordering.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const Record& record) = 0;
    virtual void repeated(const Record& record) { (void)record; }
    virtual void flush() {}
    std::atomic<Level> level{Level::Info};
};

// The capture threshold: records below it are not formatted, stored or sent.
void setLevel(Level level) noexcept;
[[nodiscard]] Level level() noexcept;

// The frame stamped on records from now on (the renderer sets it as it starts a frame).
void setFrame(uint64_t frame) noexcept;

void addSink(std::shared_ptr<Sink> sink);
void removeSink(const Sink* sink);
// The default stderr sink ("I render: message"); remove it to keep stderr quiet.
[[nodiscard]] const std::shared_ptr<Sink>& stderrSink();
void flush();   // sinks print what they hold back (a pending repeat count)

struct Page {
    std::vector<Record> lines;
    uint64_t next = 0;      // pass as `since` to continue
    uint64_t dropped = 0;   // records after `since` that fell off the ring before this read
};

// Records with seq > since, at or above minLevel, on `channel` if given, at most `limit`.
[[nodiscard]] Page read(uint64_t since, Level minLevel = Level::Info,
                        std::optional<Channel> channel = std::nullopt, std::size_t limit = 50);

struct Stats {
    uint64_t written = 0;   // records, not counting repeats
    uint64_t repeats = 0;   // calls folded into an existing record
    uint64_t newest = 0;    // seq of the newest record (0: none)
    uint64_t oldest = 0;    // seq of the oldest record still in the ring
};
[[nodiscard]] Stats stats();

// Empties the ring and restarts seq at 1 (tests and benchmarks).
void clear();

// Writes an already formatted message (cut at kMaxMessage). Checks the threshold itself.
void write(Level level, Channel channel, std::string_view msg);

namespace detail {
inline std::atomic<uint8_t> g_threshold{static_cast<uint8_t>(Level::Info)};

template <class... Args>
void emit(Level level, Channel channel, std::format_string<Args...> fmt, Args&&... args) {
    // A few bytes past the limit, so write() can cut on a whole UTF-8 character.
    constexpr std::size_t cap = kMaxMessage + 4;
    thread_local char buf[cap];
    auto r = std::format_to_n(buf, static_cast<std::ptrdiff_t>(cap), fmt,
                              std::forward<Args>(args)...);
    std::size_t n = r.size < static_cast<std::ptrdiff_t>(cap) ? static_cast<std::size_t>(r.size)
                                                              : cap;
    write(level, channel, std::string_view(buf, n));
}
} // namespace detail

[[nodiscard]] inline bool enabled(Level level) noexcept {
    return static_cast<uint8_t>(level) >= detail::g_threshold.load(std::memory_order_relaxed);
}

} // namespace ohao::log

#define OHAO_LOG(level, channel, ...)                                                      \
    do {                                                                                   \
        if (::ohao::log::enabled(::ohao::log::Level::level))                               \
            ::ohao::log::detail::emit(::ohao::log::Level::level,                           \
                                      ::ohao::log::Channel::channel, __VA_ARGS__);         \
    } while (0)
