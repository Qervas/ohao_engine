// Test 6 of sub-project 4 (docs/specs/2026-10-23-agent-control-surface-design.md, "Logging"):
// level filtering, ring wrap and `dropped`, repeat collapse, channel filter, `since`
// paging, concurrent writers, sink routing, the 1 KB cut and the stderr format.
#include "core/log.hpp"
#include "common/harness.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define FILENO _fileno
#else
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define FILENO fileno
#endif

using namespace ohao;
using log::Channel;
using log::Level;

namespace {

// Collects what reaches it.
struct CaptureSink final : log::Sink {
    std::vector<log::Record> written;
    std::vector<uint32_t> repeats;
    void write(const log::Record& r) override { written.push_back(r); }
    void repeated(const log::Record& r) override { repeats.push_back(r.repeat); }
};

void reset(Level threshold = Level::Info) {
    log::setLevel(threshold);
    log::setFrame(0);
    log::clear();
}

void sp4_t06_level_filtering() {
    reset(Level::Warn);
    int evaluated = 0;
    auto count = [&] { return ++evaluated; };
    OHAO_LOG(Info, render, "below {}", count());
    OHAO_LOG(Debug, render, "below {}", count());
    CHECK(evaluated == 0, "arguments of a record below the threshold are not evaluated");
    OHAO_LOG(Warn, render, "at {}", count());
    OHAO_LOG(Error, gpu, "above {}", count());
    CHECK(evaluated == 2, "arguments at or above the threshold are evaluated once");
    auto page = log::read(0, Level::Trace);
    CHECK(page.lines.size() == 2, "only warn and error were stored");
    CHECK(page.lines[0].msg == "at 1" && page.lines[0].level == Level::Warn, "warn record");
    CHECK(page.lines[1].msg == "above 2" && page.lines[1].channel == Channel::gpu, "error record");

    reset(Level::Off);
    OHAO_LOG(Error, render, "off");
    CHECK(log::stats().written == 0, "off stores nothing");

    reset(Level::Trace);
    OHAO_LOG(Trace, ecs, "trace");
    CHECK(log::stats().written == 1, "trace stores trace");
    CHECK(log::read(0).lines.empty(), "read's default minimum level is info");
    reset();
}

void sp4_t06_ring_wrap_and_dropped() {
    reset();
    const uint64_t total = log::kRingCapacity + 904;
    for (uint64_t i = 1; i <= total; ++i) OHAO_LOG(Info, asset, "record {}", i);
    auto st = log::stats();
    CHECK(st.written == total && st.newest == total, "every record took a seq");
    CHECK(st.oldest == 905, "the ring keeps the newest 4096");

    auto page = log::read(0, Level::Info, std::nullopt, 10);
    CHECK(page.dropped == 904, "dropped counts the records after `since` that fell off");
    CHECK(page.lines.size() == 10 && page.lines.front().seq == 905, "reading resumes at the oldest");
    CHECK(page.lines.front().msg == "record 905", "the oldest kept record is intact");
    CHECK(page.next == 914, "next is the last seq returned");

    auto later = log::read(1000, Level::Info, std::nullopt, 1);
    CHECK(later.dropped == 0 && later.lines.front().seq == 1001, "a since still in the ring drops nothing");

    auto end = log::read(total, Level::Info);
    CHECK(end.lines.empty() && end.next == total, "reading at the head returns nothing and keeps next");
    reset();
}

void sp4_t06_repeat_collapse() {
    reset();
    auto sink = std::make_shared<CaptureSink>();
    log::addSink(sink);
    for (int i = 0; i < 3; ++i) OHAO_LOG(Warn, physics, "same {}", 1);
    OHAO_LOG(Error, physics, "same {}", 1);     // other level: a new record
    OHAO_LOG(Error, audio, "same {}", 1);       // other channel: a new record
    OHAO_LOG(Error, audio, "same {}", 1);
    OHAO_LOG(Warn, physics, "same {}", 1);      // not consecutive: a new record
    log::removeSink(sink.get());

    auto page = log::read(0, Level::Trace);
    CHECK(page.lines.size() == 4, "seven calls give four records");
    CHECK(page.lines[0].repeat == 3, "three identical calls collapse into one record");
    CHECK(page.lines[1].repeat == 1 && page.lines[2].repeat == 2 && page.lines[3].repeat == 1,
          "level, channel and adjacency decide what collapses");
    auto st = log::stats();
    CHECK(st.written == 4 && st.repeats == 3, "stats count records and repeats apart");
    CHECK(sink->written.size() == 4, "a sink sees each record once");
    CHECK((sink->repeats == std::vector<uint32_t>{2, 3, 2}), "and each repeat with the count so far");
    reset();
}

void sp4_t06_channel_filter() {
    reset();
    OHAO_LOG(Info, render, "r1");
    OHAO_LOG(Info, gpu, "g1");
    OHAO_LOG(Warn, validation, "v1");
    OHAO_LOG(Info, gpu, "g2");
    OHAO_LOG(Debug, gpu, "below threshold");
    auto gpu = log::read(0, Level::Info, Channel::gpu);
    CHECK(gpu.lines.size() == 2 && gpu.lines[0].msg == "g1" && gpu.lines[1].msg == "g2", "gpu only");
    CHECK(gpu.next == 4, "next is the last seq examined, past filtered records");
    auto warn = log::read(0, Level::Warn);
    CHECK(warn.lines.size() == 1 && warn.lines[0].channel == Channel::validation, "level filter");
    auto none = log::read(0, Level::Info, Channel::rpc);
    CHECK(none.lines.empty() && none.next == 4, "an empty channel returns nothing and advances");
    reset();
}

void sp4_t06_since_paging() {
    reset();
    for (int i = 1; i <= 120; ++i) OHAO_LOG(Info, api, "line {}", i);
    std::vector<std::string> seen;
    uint64_t since = 0;
    int pages = 0;
    for (;;) {
        auto page = log::read(since, Level::Info, std::nullopt, 50);
        if (page.lines.empty()) break;
        ++pages;
        for (auto& r : page.lines) seen.push_back(r.msg);
        CHECK(page.next == page.lines.back().seq, "next is the last line's seq");
        since = page.next;
    }
    CHECK(pages == 3, "120 lines are three pages of 50");
    CHECK(seen.size() == 120 && seen.front() == "line 1" && seen.back() == "line 120", "in order, none twice");
    OHAO_LOG(Info, api, "line 121");
    auto tail = log::read(since);
    CHECK(tail.lines.size() == 1 && tail.lines[0].msg == "line 121", "a later read sees only the new line");
    reset();
}

void sp4_t06_concurrent_writers() {
    reset();
    constexpr int kThreads = 8, kEach = 500;   // 4,000 records: all fit in the ring
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < kEach; ++i)
                OHAO_LOG(Info, ecs, "thread {:02} item {:04} {:->48}", t, i, '|');
        });
    for (auto& th : threads) th.join();

    auto page = log::read(0, Level::Info, std::nullopt, kThreads * kEach + 1);
    CHECK(page.lines.size() == static_cast<std::size_t>(kThreads * kEach), "every call is one record");
    std::vector<int> nextItem(kThreads, 0);
    for (std::size_t k = 0; k < page.lines.size(); ++k) {
        const auto& r = page.lines[k];
        CHECK(r.seq == k + 1, "seqs are contiguous");
        int t = -1, i = -1;
        CHECK(std::sscanf(r.msg.c_str(), "thread %d item %d", &t, &i) == 2, "record parses");
        CHECK(t >= 0 && t < kThreads, "thread id in range");
        CHECK(r.msg.size() == 68 && r.msg.back() == '|', "no torn record: full length, intact tail");
        CHECK(i == nextItem[t], "each thread's records are in its own order, none lost");
        ++nextItem[t];
    }
    reset();
}

void sp4_t06_sink_routing() {
    reset(Level::Debug);
    auto warnSink = std::make_shared<CaptureSink>();
    auto debugSink = std::make_shared<CaptureSink>();
    warnSink->level = Level::Warn;
    debugSink->level = Level::Debug;
    log::addSink(warnSink);
    log::addSink(debugSink);
    OHAO_LOG(Debug, render, "d");
    OHAO_LOG(Info, render, "i");
    OHAO_LOG(Warn, render, "w");
    OHAO_LOG(Error, render, "e");
    CHECK(warnSink->written.size() == 2 && warnSink->written[0].msg == "w", "a warn sink sees warn and error");
    CHECK(debugSink->written.size() == 4, "a debug sink sees all four");
    log::removeSink(warnSink.get());
    OHAO_LOG(Error, render, "after removal");
    CHECK(warnSink->written.size() == 2, "a removed sink sees nothing more");
    CHECK(debugSink->written.size() == 5, "the others keep receiving");
    log::removeSink(debugSink.get());

    log::setLevel(Level::Warn);
    auto infoSink = std::make_shared<CaptureSink>();
    infoSink->level = Level::Info;
    log::addSink(infoSink);
    OHAO_LOG(Info, render, "below the capture threshold");
    CHECK(infoSink->written.empty(), "the capture threshold applies before any sink");
    log::removeSink(infoSink.get());
    reset();
}

void sp4_t06_frame_time_and_cut() {
    reset();
    log::setFrame(7);
    OHAO_LOG(Info, runtime, "stamped");
    log::setFrame(8);
    OHAO_LOG(Info, runtime, "stamped later");
    auto page = log::read(0);
    CHECK(page.lines[0].frame == 7 && page.lines[1].frame == 8, "records carry the frame set before them");
    CHECK(page.lines[1].t >= page.lines[0].t && page.lines[0].t >= 0.0, "time since start, non-decreasing");

    reset();
    OHAO_LOG(Info, asset, "{}", std::string(1500, 'a'));
    std::string accents;
    for (int i = 0; i < 700; ++i) accents += "\xC3\xA9";   // é, two bytes each
    OHAO_LOG(Info, asset, "{}", accents);
    log::write(Level::Info, Channel::asset, std::string(5000, 'b'));
    page = log::read(0);
    CHECK(page.lines[0].msg.size() == log::kMaxMessage, "an ASCII message is cut at 1 KB");
    CHECK(page.lines[1].msg.size() == log::kMaxMessage &&
              (static_cast<unsigned char>(page.lines[1].msg.back()) & 0xC0) == 0x80 &&
              static_cast<unsigned char>(page.lines[1].msg[page.lines[1].msg.size() - 2]) == 0xC3,
          "a UTF-8 message is cut on a whole character");
    CHECK(page.lines[2].msg.size() == log::kMaxMessage, "log::write cuts too");

    std::string odd = "x";
    for (int i = 0; i < 700; ++i) odd += "\xC3\xA9";        // the 1,024th byte starts a character
    OHAO_LOG(Info, asset, "{}", odd);
    page = log::read(3);
    CHECK(page.lines[0].msg.size() == log::kMaxMessage - 1, "the cut backs off a split character");
    reset();
}

void sp4_t06_names() {
    for (int i = 0; i <= static_cast<int>(Level::Off); ++i) {
        auto l = static_cast<Level>(i);
        CHECK(log::parseLevel(log::levelName(l)) == l, "level names round-trip");
    }
    for (int i = 0; i < static_cast<int>(Channel::Count); ++i) {
        auto c = static_cast<Channel>(i);
        CHECK(log::parseChannel(log::channelName(c)) == c, "channel names round-trip");
    }
    CHECK(log::parseLevel("warning") == Level::Warn, "warning is warn");
    CHECK(!log::parseLevel("loud") && !log::parseChannel("net"), "unknown names are rejected");
    CHECK(log::channelName(Channel::validation) == "validation", "the validation channel");
}

void sp4_t06_stderr_format() {
    reset();
    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() /
                          ("ohao_log_stderr_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                           ".txt");
    std::fflush(stderr);
    const int saved = DUP(FILENO(stderr));
    std::FILE* f = std::fopen(file.string().c_str(), "wb");
    CHECK(f && saved >= 0, "redirect stderr");
    DUP2(FILENO(f), FILENO(stderr));
    log::addSink(log::stderrSink());
    OHAO_LOG(Warn, validation, "VUID-x | text {}", 1);
    OHAO_LOG(Warn, validation, "VUID-x | text {}", 1);
    OHAO_LOG(Warn, validation, "VUID-x | text {}", 1);
    OHAO_LOG(Info, gpu, "ready");
    OHAO_LOG(Error, render, "boom");
    OHAO_LOG(Error, render, "boom");
    log::flush();
    log::removeSink(log::stderrSink().get());
    std::fflush(stderr);
    DUP2(saved, FILENO(stderr));
    CLOSE(saved);
    std::fclose(f);

    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    fs::remove(file);
    std::string text = ss.str();
    std::string expected =
        "W validation: VUID-x | text 1\n"
        "W validation: ... repeated 2 more times\n"
        "I gpu: ready\n"
        "E render: boom\n"
        "E render: ... repeated 1 more times\n";
    std::string norm;
    for (char c : text) if (c != '\r') norm += c;
    CHECK(norm == expected, "stderr lines are '<L> <channel>: <msg>' with repeats folded");
    reset();
}

} // namespace

int main() {
    // The default stderr sink stays off while the suite floods the logger.
    log::removeSink(log::stderrSink().get());
    std::printf("log_tests (sub-project 4, test 6)\n");
    RUN(sp4_t06_level_filtering);
    RUN(sp4_t06_ring_wrap_and_dropped);
    RUN(sp4_t06_repeat_collapse);
    RUN(sp4_t06_channel_filter);
    RUN(sp4_t06_since_paging);
    RUN(sp4_t06_concurrent_writers);
    RUN(sp4_t06_sink_routing);
    RUN(sp4_t06_frame_time_and_cut);
    RUN(sp4_t06_names);
    RUN(sp4_t06_stderr_format);
    return ohao::test::finish("log_tests");
}
