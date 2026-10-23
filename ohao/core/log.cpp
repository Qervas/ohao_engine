#include "core/log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>

namespace ohao::log {
namespace {

constexpr std::string_view kLevelNames[] = {"trace", "debug", "info", "warn", "error", "off"};
constexpr char kLevelLetters[] = {'T', 'D', 'I', 'W', 'E', '-'};
constexpr std::string_view kChannelNames[] = {"render", "gpu", "validation", "ecs", "asset",
                                              "physics", "audio", "api", "rpc", "runtime"};
static_assert(std::size(kChannelNames) == static_cast<std::size_t>(Channel::Count));

// "I render: message", one fwrite per line. A run of identical records prints once, then
// "... repeated N more times" before the next line (or at flush).
class StderrSink final : public Sink {
public:
    void write(const Record& r) override {
        flushPending();
        line_.clear();
        std::format_to(std::back_inserter(line_), "{} {}: {}\n",
                       kLevelLetters[static_cast<int>(r.level)], channelName(r.channel), r.msg);
        std::fwrite(line_.data(), 1, line_.size(), stderr);
    }
    void repeated(const Record& r) override {
        pendingLevel_ = r.level;
        pendingChannel_ = r.channel;
        pendingMore_ = r.repeat - 1;
    }
    void flush() override { flushPending(); }

private:
    void flushPending() {
        if (pendingMore_ == 0) return;
        line_.clear();
        std::format_to(std::back_inserter(line_), "{} {}: ... repeated {} more times\n",
                       kLevelLetters[static_cast<int>(pendingLevel_)],
                       channelName(pendingChannel_), pendingMore_);
        std::fwrite(line_.data(), 1, line_.size(), stderr);
        pendingMore_ = 0;
    }

    std::string line_;
    Level pendingLevel_ = Level::Info;
    Channel pendingChannel_ = Channel::render;
    uint32_t pendingMore_ = 0;
};

struct State {
    std::mutex mutex;
    std::vector<Record> ring{kRingCapacity};
    uint64_t next = 1;       // seq of the next new record
    uint64_t repeats = 0;
    std::vector<std::shared_ptr<Sink>> sinks;
    std::shared_ptr<Sink> stderrSink = std::make_shared<StderrSink>();
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    State() {
        if (const char* env = std::getenv("OHAO_LOG_LEVEL")) {
            if (auto l = parseLevel(env)) {
                detail::g_threshold.store(static_cast<uint8_t>(*l), std::memory_order_relaxed);
                stderrSink->level.store(*l == Level::Off ? Level::Error : *l);
            }
        }
        sinks.push_back(stderrSink);
        std::atexit([] { ohao::log::flush(); });
    }
};

// Never destroyed: engine objects may log from their destructors during static teardown.
State& state() {
    static State* s = new State;
    return *s;
}

[[maybe_unused]] const bool kStarted = (state(), true);   // read OHAO_LOG_LEVEL at start-up

std::atomic<uint64_t> g_frame{0};

std::string_view cut(std::string_view msg) {
    if (msg.size() <= kMaxMessage) return msg;
    std::size_t n = kMaxMessage;
    while (n > 0 && (static_cast<unsigned char>(msg[n]) & 0xC0) == 0x80) --n;   // whole UTF-8
    return msg.substr(0, n);
}

} // namespace

std::string_view levelName(Level level) noexcept {
    auto i = static_cast<std::size_t>(level);
    return i < std::size(kLevelNames) ? kLevelNames[i] : "?";
}

std::string_view channelName(Channel channel) noexcept {
    auto i = static_cast<std::size_t>(channel);
    return i < std::size(kChannelNames) ? kChannelNames[i] : "?";
}

std::optional<Level> parseLevel(std::string_view name) noexcept {
    for (std::size_t i = 0; i < std::size(kLevelNames); ++i)
        if (name == kLevelNames[i]) return static_cast<Level>(i);
    if (name == "warning") return Level::Warn;
    return std::nullopt;
}

std::optional<Channel> parseChannel(std::string_view name) noexcept {
    for (std::size_t i = 0; i < std::size(kChannelNames); ++i)
        if (name == kChannelNames[i]) return static_cast<Channel>(i);
    return std::nullopt;
}

void setLevel(Level level) noexcept {
    state();
    detail::g_threshold.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
}

Level level() noexcept {
    return static_cast<Level>(detail::g_threshold.load(std::memory_order_relaxed));
}

void setFrame(uint64_t frame) noexcept { g_frame.store(frame, std::memory_order_relaxed); }

void addSink(std::shared_ptr<Sink> sink) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.sinks.push_back(std::move(sink));
}

void removeSink(const Sink* sink) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    for (auto it = s.sinks.begin(); it != s.sinks.end(); ++it) {
        if (it->get() == sink) {
            (*it)->flush();
            s.sinks.erase(it);
            return;
        }
    }
}

const std::shared_ptr<Sink>& stderrSink() { return state().stderrSink; }

void flush() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    for (auto& sink : s.sinks) sink->flush();
}

void write(Level level, Channel channel, std::string_view msg) {
    if (!enabled(level)) return;
    msg = cut(msg);
    State& s = state();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
    const uint64_t frame = g_frame.load(std::memory_order_relaxed);

    std::lock_guard lock(s.mutex);
    if (s.next > 1) {
        Record& last = s.ring[(s.next - 2) % kRingCapacity];
        if (last.level == level && last.channel == channel && last.msg == msg) {
            ++last.repeat;
            ++s.repeats;
            for (auto& sink : s.sinks)
                if (level >= sink->level.load(std::memory_order_relaxed)) sink->repeated(last);
            return;
        }
    }
    Record& r = s.ring[(s.next - 1) % kRingCapacity];
    r.seq = s.next++;
    r.t = t;
    r.frame = frame;
    r.level = level;
    r.channel = channel;
    r.repeat = 1;
    r.msg.assign(msg);
    for (auto& sink : s.sinks)
        if (level >= sink->level.load(std::memory_order_relaxed)) sink->write(r);
}

Page read(uint64_t since, Level minLevel, std::optional<Channel> channel, std::size_t limit) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    Page page;
    const uint64_t newest = s.next - 1;
    const uint64_t oldest = newest > kRingCapacity ? newest - kRingCapacity + 1 : 1;
    uint64_t seq = since + 1;
    if (seq < oldest) {
        page.dropped = oldest - seq;
        seq = oldest;
    }
    for (; seq <= newest && page.lines.size() < limit; ++seq) {
        const Record& r = s.ring[(seq - 1) % kRingCapacity];
        if (r.level < minLevel || (channel && r.channel != *channel)) continue;
        page.lines.push_back(r);
    }
    page.next = seq - 1;
    return page;
}

Stats stats() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    Stats st;
    st.written = s.next - 1;
    st.repeats = s.repeats;
    st.newest = s.next - 1;
    st.oldest = st.newest == 0 ? 0 : (st.newest > kRingCapacity ? st.newest - kRingCapacity + 1 : 1);
    return st;
}

void clear() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    for (auto& sink : s.sinks) sink->flush();
    s.next = 1;
    s.repeats = 0;
}

} // namespace ohao::log
