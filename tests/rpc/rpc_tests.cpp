// ohao_rpc tests (sub-project 4, step 5): test 7 (JSON-RPC framing, ordering, errors,
// cancellation) and test 8 (the MCP adapter), against a server on an in-memory transport.
// CPU only. tests/serve/check_serve.py covers the same server as a process (tests 8-10, 15).
#include "api/session.hpp"
#include "common/harness.hpp"
#include "core/log.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "reflect/json_writer.hpp"
#include "rpc/server.hpp"
#include "rpc/transport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ohao;
using reflect::JsonDocument;
using reflect::JsonNode;
using reflect::JsonType;

namespace {

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

// Bytes in, lines out: the test feeds chunks (split wherever it likes) and closes; the
// server's writes are collected as lines.
class ScriptTransport final : public rpc::Transport {
public:
    std::size_t read(char* buf, std::size_t cap) override {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [&] { return !m_in.empty() || m_closed; });
        if (m_in.empty()) return 0;
        std::string& chunk = m_in.front();
        const std::size_t n = std::min(cap, chunk.size());
        std::memcpy(buf, chunk.data(), n);
        chunk.erase(0, n);
        if (chunk.empty()) m_in.pop_front();
        return n;
    }
    void write(std::string_view line) override {
        std::lock_guard lock(m_mutex);
        m_out.emplace_back(line);
        m_cv.notify_all();
    }
    void feed(std::string chunk) {
        std::lock_guard lock(m_mutex);
        m_in.push_back(std::move(chunk));
        m_cv.notify_all();
    }
    void close() {
        std::lock_guard lock(m_mutex);
        m_closed = true;
        m_cv.notify_all();
    }
    // Waits until a line contains `needle` (true) or `ms` pass (false).
    bool waitFor(std::string_view needle, int ms = 20000) {
        std::unique_lock lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(ms), [&] {
            for (const auto& l : m_out)
                if (l.find(needle) != std::string::npos) return true;
            return false;
        });
    }
    std::vector<std::string> lines() {
        std::lock_guard lock(m_mutex);
        return m_out;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_in;
    std::vector<std::string> m_out;
    bool m_closed{false};
};

// A session on its own world with a server running on another thread.
struct Rig {
    ecs::World world;
    api::Session session{world, engineTypes()};
    ScriptTransport io;
    rpc::Server server;
    std::thread thread;

    explicit Rig(rpc::ServerOptions o = {}) : server(session, io, o) {
        session.setHostKind(api::HostKind::Headless);
    }
    void start() {
        thread = std::thread([this] { server.run(); });
    }
    // Waits for the reply to request `lastId` (raw, as sent; empty: no wait), then closes
    // the input (end of input drops anything still queued) and waits for the server to
    // return. The written lines.
    std::vector<std::string> finish(std::string_view lastId = {}) {
        if (!lastId.empty()) (void)io.waitFor("\"id\":" + std::string(lastId) + ",");
        io.close();
        if (thread.joinable()) thread.join();
        return io.lines();
    }
    ~Rig() {
        io.close();
        if (thread.joinable()) thread.join();
    }
};

struct Msg {
    std::string text;
    JsonDocument doc;
    bool ok{false};
    explicit Msg(std::string t) : text(std::move(t)) {
        auto r = ecs::parseJson(text);
        ok = static_cast<bool>(r);
        if (ok) doc = std::move(r.value());
    }
    const JsonNode* at(std::initializer_list<std::string_view> path) const {
        if (!ok) return nullptr;
        const JsonNode* n = &doc.root();
        for (auto k : path) {
            if (!n || n->type != JsonType::Object) return nullptr;
            n = doc.member(*n, k);
        }
        return n;
    }
    std::string str(std::initializer_list<std::string_view> path) const {
        const JsonNode* n = at(path);
        return n && (n->type == JsonType::String || n->type == JsonType::Number) ? std::string(n->text) : "";
    }
    // The raw text of the id member, as written on the wire.
    std::string rawId() const {
        const auto at = text.find("\"id\":");
        if (at == std::string::npos) return "";
        const auto start = at + 5;
        if (text[start] == '"') {
            std::size_t j = start + 1;
            while (text[j] != '"') j += text[j] == '\\' ? 2 : 1;
            return text.substr(start, j + 1 - start);
        }
        return text.substr(start, text.find_first_of(",}", start) - start);
    }
};

std::string req(std::string_view id, std::string_view method, std::string_view params = "{}") {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) + ",\"method\":\"" + std::string(method) +
           "\",\"params\":" + std::string(params) + "}\n";
}

// The reply whose raw id is `id`, or an empty Msg.
Msg reply(const std::vector<std::string>& lines, std::string_view id) {
    for (const auto& l : lines) {
        Msg m(l);
        if (m.ok && m.at({"method"}) == nullptr && m.rawId() == id) return m;
    }
    return Msg("");
}

std::size_t countReplies(const std::vector<std::string>& lines) {
    std::size_t n = 0;
    for (const auto& l : lines) n += Msg(l).at({"method"}) == nullptr;
    return n;
}

// world.apply params creating `n` spinning entities: enough work that a long world.step
// runs for seconds and crosses the 250 ms progress interval.
std::string spinners(int n) {
    std::string s = R"({"commands":[)";
    for (int i = 0; i < n; ++i)
        s += std::string(i ? "," : "") +
             R"({"op":"entity.create","name":"S","components":{"Spin":{"degreesPerSecond":30}}})";
    return s + "]}";
}

// ---- test 7: framing, ordering, errors ------------------------------------------------------

void sp4_t07_crlf_and_reads_split_anywhere() {
    Rig r;
    r.start();
    const std::string a = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"world.info\"}\r\n";
    for (char c : a) r.io.feed(std::string(1, c));   // one byte per read
    const std::string b = req("2", "world.info") + req("3", "engine.info");
    r.io.feed(b.substr(0, 17));
    r.io.feed(b.substr(17, 60));
    r.io.feed(b.substr(77));
    const auto lines = r.finish("3");
    CHECK(lines.size() == 3, "three requests, three replies");
    CHECK(reply(lines, "1").str({"result", "host"}) == "headless", "CRLF and byte-by-byte reads");
    CHECK(Msg(lines[1]).rawId() == "2" && Msg(lines[2]).rawId() == "3", "replies in arrival order");
    for (const auto& l : lines) CHECK(l.back() == '\n' && l.find('\r') == std::string::npos, "LF-terminated, compact");
}

void sp4_t07_line_reader_skips_oversize_lines() {
    ScriptTransport t;
    t.feed(std::string(40, 'x'));
    t.feed(std::string(40, 'y') + "\nshort\n");
    t.feed(std::string(25, 'z') + "\r\nlast");
    t.close();
    rpc::LineReader lines(t, 30, 7);
    std::string line;
    CHECK(lines.next(line) == rpc::LineReader::Status::Oversize, "an 80-byte line over a 30-byte limit, read 7 at a time");
    CHECK(lines.next(line) == rpc::LineReader::Status::Line && line == "short", "the next line is intact");
    CHECK(lines.next(line) == rpc::LineReader::Status::Line && line == std::string(25, 'z'), "CR dropped");
    CHECK(lines.next(line) == rpc::LineReader::Status::Line && line == "last", "a final line without LF");
    CHECK(lines.next(line) == rpc::LineReader::Status::End, "then the end");
}

void sp4_t07_errors_follow_the_table() {
    rpc::ServerOptions o;
    o.maxMessage = 256;
    Rig r(o);
    r.session.setRenderHandler([](const api::RenderRequest&) {
        api::RenderOutcome out;
        reflect::Diagnostic d;
        d.code = "E_RENDER";
        d.message = "no GPU in this test";
        out.errors.push_back(d);
        return out;
    });
    r.start();
    r.io.feed("{not json\n");
    r.io.feed("42\n");
    r.io.feed("[" + req("9", "world.info").substr(0, req("9", "world.info").size() - 1) + "]\n");
    r.io.feed("{\"id\":4,\"method\":\"world.info\"}\n");
    r.io.feed(std::string(300, ' ') + req("5", "world.info"));   // over 256 bytes
    r.io.feed(req("6", "world.inof"));
    r.io.feed(req("7", "world.step", R"({"frames":"x"})"));
    r.io.feed(req("8", "world.restore", R"({"name":"nope"})"));
    r.io.feed(req("10", "render.capture", R"({"path":"x.png"})"));
    r.io.feed(req("11", "world.step", R"({"frames":1000001})"));
    r.io.feed(req("12", "world.info", "[1]"));
    const auto lines = r.finish("12");
    CHECK(lines.size() == 11, "every malformed or failing message is answered once");
    const auto code = [&](std::size_t i) { return Msg(lines[i]).str({"error", "code"}); };
    CHECK(code(0) == "-32700" && Msg(lines[0]).rawId() == "null", "invalid JSON: -32700, id null");
    CHECK(code(1) == "-32600" && code(2) == "-32600", "a non-object and a batch: -32600");
    CHECK(code(3) == "-32600" && Msg(lines[3]).rawId() == "4", "no \"jsonrpc\": -32600 with the id");
    CHECK(code(4) == "-32600" && Msg(lines[4]).rawId() == "null", "an oversize line: -32600");
    const Msg unknown = reply(lines, "6");
    CHECK(unknown.str({"error", "code"}) == "-32601" && unknown.str({"error", "data", "code"}) == "E_METHOD" &&
              unknown.str({"error", "data", "suggestion"}) == "world.info",
          "an unknown method: -32601 with a suggestion");
    const Msg params = reply(lines, "7");
    CHECK(params.str({"error", "code"}) == "-32602" && params.at({"error", "data"})->type == JsonType::Array,
          "bad params: -32602 with the diagnostics");
    const Msg domain = reply(lines, "8");
    CHECK(domain.str({"error", "code"}) == "1" && domain.str({"error", "data", "code"}) == "E_SNAPSHOT_UNKNOWN",
          "a domain error: its exit class (1) and code");
    CHECK(reply(lines, "10").str({"error", "code"}) == "3" &&
              reply(lines, "10").str({"error", "data", "code"}) == "E_RENDER",
          "a GPU error: class 3");
    CHECK(reply(lines, "11").str({"error", "code"}) == "-32602", "the step cap is a params error");
    CHECK(reply(lines, "12").str({"error", "code"}) == "-32602", "params must be an object");
}

void sp4_t07_ids_echo_byte_for_byte_and_notifications_get_no_reply() {
    Rig r;
    r.start();
    const char* ids[] = {R"("aAb")", "-7", "9223372036854775808", "18446744073709551616", "1.5e3", R"("")"};
    for (const char* id : ids) r.io.feed(req(id, "world.info"));
    r.io.feed("{\"jsonrpc\":\"2.0\",\"method\":\"world.step\",\"params\":{\"frames\":2}}\n");   // a notification
    r.io.feed(req("\"after\"", "world.info"));
    const auto lines = r.finish("\"after\"");
    CHECK(lines.size() == std::size(ids) + 1, "a notification gets no reply");
    for (std::size_t i = 0; i < std::size(ids); ++i) CHECK(Msg(lines[i]).rawId() == ids[i], "ids echo byte for byte");
    CHECK(reply(lines, "\"after\"").str({"result", "frame"}) == "2", "but it ran (2 frames)");
}

void sp4_t07_cancellation_and_ping_while_busy() {
    Rig r;
    CHECK(r.session.call("world.apply", spinners(2000)).rfind("{\"result\"", 0) == 0, "2000 spinning entities");
    r.start();
    r.io.feed(req("\"long\"", "world.step", R"({"frames":1000000,"_meta":{"progressToken":"tok"}})"));
    r.io.feed(req("\"queued\"", "world.step", R"({"frames":5})"));
    CHECK(r.io.waitFor("notifications/progress"), "progress arrives while stepping");
    r.io.feed(req("\"ping\"", "ping"));
    CHECK(r.io.waitFor("\"id\":\"ping\""), "ping is answered while the engine is busy");
    r.io.feed("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":\"queued\"}}\n");
    r.io.feed("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":\"long\"}}\n");
    r.io.feed(req("\"info\"", "world.info"));
    CHECK(r.io.waitFor("\"id\":\"info\""), "the next request runs");
    const auto lines = r.finish("\"info\"");
    CHECK(reply(lines, "\"long\"").text.empty() && reply(lines, "\"queued\"").text.empty(),
          "cancelled requests (running and queued) get no response");
    const Msg progress(lines.front());
    CHECK(progress.str({"method"}) == "notifications/progress" && progress.str({"params", "progressToken"}) == "tok" &&
              progress.str({"params", "total"}) == "1000000",
          "progress names the token and the total");
    const uint64_t frame = std::stoull(reply(lines, "\"info\"").str({"result", "frame"}));
    CHECK(frame > 0 && frame < 1000000 && frame == r.session.frame(), "the world stopped at a whole step");
}

void sp4_t07_end_of_input_cancels_and_returns() {
    Rig r;
    CHECK(r.session.call("world.apply", spinners(2000)).rfind("{\"result\"", 0) == 0, "2000 spinning entities");
    r.start();
    r.io.feed(req("1", "world.step", R"({"frames":1000000,"_meta":{"progressToken":1}})"));
    CHECK(r.io.waitFor("notifications/progress"), "stepping");
    const auto t0 = std::chrono::steady_clock::now();
    const auto lines = r.finish();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5), "EOF stops the running request");
    CHECK(reply(lines, "1").text.empty() && r.session.frame() < 1000000, "without answering it");
}

// ---- test 8: MCP ----------------------------------------------------------------------------

void sp4_t08_initialize_negotiates_the_version() {
    Rig r;
    r.start();
    r.io.feed(req("1", "initialize", R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t"}})"));
    r.io.feed(req("2", "initialize", R"({"protocolVersion":"2025-11-25"})"));
    r.io.feed(req("3", "initialize", R"({"protocolVersion":"1999-01-01"})"));
    r.io.feed("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
    r.io.feed(req("4", "ping"));
    (void)r.io.waitFor("\"id\":3,");
    const auto lines = r.finish("4");
    CHECK(lines.size() == 4, "notifications/initialized gets no reply");
    CHECK(reply(lines, "1").str({"result", "protocolVersion"}) == "2025-06-18", "2025-06-18 is kept");
    CHECK(reply(lines, "2").str({"result", "protocolVersion"}) == "2025-11-25", "2025-11-25 is kept");
    CHECK(reply(lines, "3").str({"result", "protocolVersion"}) == "2025-11-25", "an unknown one gets the newest");
    const Msg m = reply(lines, "1");
    CHECK(m.at({"result", "capabilities", "tools"}) && m.at({"result", "capabilities", "logging"}),
          "tools and logging capabilities");
    CHECK(m.str({"result", "serverInfo", "name"}) == "ohao", "serverInfo");
    CHECK(m.str({"result", "instructions"}).size() <= 600 && !m.str({"result", "instructions"}).empty(),
          "instructions of at most 600 characters");
}

void sp4_t08_tools_list_covers_every_method_within_budget() {
    Rig r;
    const std::string& list = r.server.toolsList();
    std::printf("\n      tools/list: %zu bytes, %zu tools", list.size(), api::methods().size());
    CHECK(list.size() <= 24 * 1024, "tools/list is at most 24 KB");
    for (const auto& m : api::methods()) {
        std::string name(m.name);
        for (char& c : name)
            if (c == '.') c = '_';
        CHECK(list.find("\"name\":\"" + name + "\"") != std::string::npos, "every method is a tool, '.' -> '_'");
    }
    CHECK(list.find("\"inputSchema\":{\"type\":\"object\"") != std::string::npos, "with an object input schema");
}

void sp4_t08_tools_call_results_and_errors() {
    Rig r;
    r.start();
    r.io.feed(req("1", "tools/call", R"({"name":"world_info"})"));
    r.io.feed(req("2", "tools/call", R"({"name":"world_restore","arguments":{"name":"nope"}})"));
    r.io.feed(req("3", "tools/call", R"({"name":"world_step","arguments":{"frames":"x"}})"));
    r.io.feed(req("4", "tools/call", R"({"name":"world_infp","arguments":{}})"));
    r.io.feed(req("5", "tools/call", R"({"name":"world_step","arguments":{"frames":3},"_meta":{"progressToken":1}})"));
    const auto lines = r.finish("5");
    const Msg ok = reply(lines, "1");
    CHECK(ok.str({"result", "isError"}).empty() && ok.at({"result", "content"}), "a result has content");
    const JsonNode* content = ok.at({"result", "content"});
    const JsonNode& block = ok.doc.child(*content, 0);
    CHECK(ok.doc.member(block, "type")->text == "text", "one text block");
    CHECK(Msg(std::string(ok.doc.member(block, "text")->text)).str({"host"}) == "headless",
          "holding the compact result JSON");
    const Msg failed = reply(lines, "2");
    CHECK(failed.at({"result", "isError"}) && failed.at({"result", "isError"})->boolean,
          "a domain error is a result with isError");
    CHECK(failed.text.find("E_SNAPSHOT_UNKNOWN") != std::string::npos, "whose text names the error");
    CHECK(reply(lines, "3").str({"error", "code"}) == "-32602", "bad arguments: -32602");
    CHECK(reply(lines, "4").str({"error", "code"}) == "-32601" &&
              reply(lines, "4").str({"error", "data", "suggestion"}) == "world_info",
          "an unknown tool: -32601 with a suggestion");
    CHECK(Msg(std::string(reply(lines, "5").doc.member(reply(lines, "5").doc.child(*reply(lines, "5").at({"result", "content"}), 0), "text")->text))
                  .str({"frame"}) == "3",
          "_meta beside the arguments is not an argument");
}

void sp4_t08_logging_set_level_sends_notifications() {
    Rig r;
    r.server.addMethod("test.log", [](std::string_view) {
        OHAO_LOG(Info, rpc, "below the client's level");
        OHAO_LOG(Warn, rpc, "rpc test warning {}", 7);
        return std::string("{}");
    });
    r.start();
    r.io.feed(req("1", "test.log"));
    r.io.feed(req("2", "logging/setLevel", R"({"level":"warning"})"));
    r.io.feed(req("3", "test.log"));
    r.io.feed(req("4", "logging/setLevel", R"({"level":"loud"})"));
    const auto lines = r.finish("4");
    std::vector<Msg> notes;
    for (const auto& l : lines)
        if (Msg m(l); m.str({"method"}) == "notifications/message") notes.push_back(std::move(m));
    CHECK(notes.size() == 1, "off until asked; then only at the level asked");
    CHECK(notes[0].str({"params", "level"}) == "warning" && notes[0].str({"params", "logger"}) == "rpc" &&
              notes[0].str({"params", "data"}) == "rpc test warning 7",
          "{level, logger: channel, data: text}");
    std::size_t noteAt = 0, replyAt = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find("notifications/message") != std::string::npos) noteAt = i;
        if (Msg(lines[i]).rawId() == "3") replyAt = i;
    }
    CHECK(noteAt < replyAt, "the notification precedes the reply of the call that logged");
    CHECK(reply(lines, "4").str({"error", "code"}) == "-32602", "an unknown level is -32602");
}

} // namespace

int main() {
    log::removeSink(log::stderrSink().get());   // the tests read the logger through the server
    std::printf("rpc_tests (sub-project 4, tests 7 and 8)\n");
    RUN(sp4_t07_crlf_and_reads_split_anywhere);
    RUN(sp4_t07_line_reader_skips_oversize_lines);
    RUN(sp4_t07_errors_follow_the_table);
    RUN(sp4_t07_ids_echo_byte_for_byte_and_notifications_get_no_reply);
    RUN(sp4_t07_cancellation_and_ping_while_busy);
    RUN(sp4_t07_end_of_input_cancels_and_returns);
    RUN(sp4_t08_initialize_negotiates_the_version);
    RUN(sp4_t08_tools_list_covers_every_method_within_budget);
    RUN(sp4_t08_tools_call_results_and_errors);
    RUN(sp4_t08_logging_set_level_sends_notifications);
    return ohao::test::finish("rpc_tests");
}
