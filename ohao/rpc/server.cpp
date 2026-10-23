#include "rpc/server.hpp"

#include "core/log.hpp"
#include "ecs/json_parse.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/schema.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace ohao::rpc {

using reflect::Diagnostic;
using reflect::JsonDocument;
using reflect::JsonNode;
using reflect::JsonType;
using reflect::JsonWriter;

namespace {

constexpr std::size_t kMaxErrorData = 20;   // diagnostics in one error's data

constexpr std::string_view kInstructions =
    "OHAO game engine. Orient with world_summary (counts, bounds, a capped outline) and world_info. Read with "
    "world_query: filter, then project fields such as [\"/Transform/position\"], with a limit. Edit with "
    "world_apply: one atomic batch of commands; keep its inverse to undo. Advance time with world_step. Load and "
    "save with scene_load and scene_save: paths are project-relative with '/', writes go under agent_out/. Look "
    "with render_capture (needs a GPU host). Read engine logs with log_read. schema_get lists every component, "
    "field, enum and error code.";
static_assert(kInstructions.size() <= 600, "the spec caps the instructions at 600 characters");

const JsonDocument& emptyObject() {
    static const JsonDocument d = ecs::parseJson("{}").value();
    return d;
}

// The raw JSON text of an id or a progress token: a number as written, a string as
// written (escapes kept, so it is echoed byte for byte), or null. False for other types.
bool rawToken(const JsonDocument& doc, const JsonNode& n, std::string& out) {
    if (n.type == JsonType::Number) {
        out.assign(n.text);
        return true;
    }
    if (n.type == JsonType::Null) {
        out = "null";
        return true;
    }
    if (n.type != JsonType::String) return false;
    const std::string_view src = doc.source;
    const std::size_t i = n.offset;
    if (i < src.size() && src[i] == '"') {
        std::size_t j = i + 1;
        while (j < src.size() && src[j] != '"') j += src[j] == '\\' ? 2 : 1;
        if (j < src.size()) {
            out.assign(src.substr(i, j + 1 - i));
            return true;
        }
    }
    JsonWriter w;
    w.string(n.text);
    out = w.take();
    return true;
}

std::string compact(const JsonDocument& doc, const JsonNode& n) {
    JsonWriter w(256);
    reflect::writeJson(w, doc, n);
    return w.take();
}

std::string resultLine(std::string_view id, std::string_view result) {
    std::string s;
    s.reserve(result.size() + id.size() + 40);
    s += R"({"jsonrpc":"2.0","id":)";
    s += id;
    s += R"(,"result":)";
    s += result;
    s += "}\n";
    return s;
}

std::string errorLine(std::string_view id, int code, std::string_view message, std::string_view data = {}) {
    JsonWriter w(256);
    w.beginObject();
    w.key("code"); w.int64(code);
    w.key("message"); w.string(message);
    if (!data.empty()) { w.key("data"); w.raw(data); }
    w.endObject();
    std::string s = R"({"jsonrpc":"2.0","id":)";
    s += id.empty() ? std::string_view("null") : id;
    s += R"(,"error":)";
    s += w.str();
    s += "}\n";
    return s;
}

std::string notificationLine(std::string_view method, std::string_view params) {
    std::string s = R"({"jsonrpc":"2.0","method":)";
    JsonWriter m;
    m.string(method);
    s += m.str();
    s += R"(,"params":)";
    s += params;
    s += "}\n";
    return s;
}

std::string messageOf(const std::vector<Diagnostic>& errors) {
    if (errors.empty()) return "the request failed";
    return errors.size() > 1 ? errors.front().message + " (+" + std::to_string(errors.size() - 1) + " more)"
                             : errors.front().message;
}

void writeDiagnostics(JsonWriter& w, const std::vector<Diagnostic>& errors) {
    w.beginArray();
    for (std::size_t i = 0; i < errors.size() && i < kMaxErrorData; ++i) reflect::writeDiagnostic(w, errors[i]);
    w.endArray();
}

std::string diagnosticsJson(const std::vector<Diagnostic>& errors) {
    JsonWriter w(256);
    writeDiagnostics(w, errors);
    return w.take();
}

std::string toolName(std::string_view method) {
    std::string s(method);
    for (char& c : s)
        if (c == '.') c = '_';
    return s;
}

// MCP log levels (RFC 5424 names) to ours and back.
std::optional<log::Level> fromMcpLevel(std::string_view l) {
    if (l == "debug") return log::Level::Debug;
    if (l == "info") return log::Level::Info;
    if (l == "notice" || l == "warning") return log::Level::Warn;
    if (l == "error" || l == "critical" || l == "alert" || l == "emergency") return log::Level::Error;
    return std::nullopt;
}

std::string_view toMcpLevel(log::Level l) {
    switch (l) {
    case log::Level::Trace: case log::Level::Debug: return "debug";
    case log::Level::Info: return "info";
    case log::Level::Warn: return "warning";
    default: return "error";
    }
}

} // namespace

struct Server::Impl {
    struct Item {
        enum class Kind : uint8_t { Request, Notification, Reply };
        Kind kind{Kind::Reply};
        std::shared_ptr<const JsonDocument> doc;
        const JsonNode* params{nullptr};   // null: {}
        std::string method;
        std::string id;              // raw JSON (requests)
        std::string progressToken;   // raw JSON, or empty
        std::string reply;           // Kind::Reply: the whole line
        bool cancelled{false};       // cancelled while queued
    };

    // Engine log records as MCP notifications, at the level the client asked for.
    class Sink final : public log::Sink {
    public:
        explicit Sink(Impl& s) : m_s(s) {}
        void write(const log::Record& r) override {
            JsonWriter w(r.msg.size() + 64);
            w.beginObject();
            w.key("level"); w.string(toMcpLevel(r.level));
            w.key("logger"); w.string(log::channelName(r.channel));
            w.key("data"); w.string(r.msg);
            w.endObject();
            m_s.send(notificationLine("notifications/message", w.str()));
        }

    private:
        Impl& m_s;
    };

    Impl(api::Session& s, Transport& t, ServerOptions o) : session(s), transport(t), options(o) {
        for (const auto& m : api::methods()) toolToMethod.emplace(toolName(m.name), std::string(m.name));
    }
    ~Impl() {
        if (sink) log::removeSink(sink.get());
    }

    api::Session& session;
    Transport& transport;
    ServerOptions options;

    std::mutex writeMutex;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::deque<std::shared_ptr<Item>> queue;
    bool eof{false};
    std::string runningId;   // the request on the engine thread (raw id), or empty
    std::atomic<bool> cancelRunning{false};
    std::thread reader;
    std::atomic<bool> readerDone{false};
    std::vector<std::string> held;   // replies waiting for flush() (holdReplies); engine thread only

    std::map<std::string, std::function<std::string(std::string_view)>, std::less<>> extra;
    std::map<std::string, std::string, std::less<>> toolToMethod;
    std::string toolsListCache;
    std::shared_ptr<Sink> sink;

    void send(std::string_view line) {
        std::lock_guard lock(writeMutex);
        transport.write(line);
    }

    // A reply from the engine thread: sent, or held for flush().
    void reply(std::string line) {
        if (options.holdReplies) held.push_back(std::move(line));
        else send(line);
    }

    void enqueue(std::shared_ptr<Item> item) {
        {
            std::lock_guard lock(queueMutex);
            queue.push_back(std::move(item));
        }
        queueReady.notify_one();
    }

    void enqueueReply(std::string line) {
        auto item = std::make_shared<Item>();
        item->kind = Item::Kind::Reply;
        item->reply = std::move(line);
        enqueue(std::move(item));
    }

    // ---- reader thread ----------------------------------------------------------------

    void readerLoop() {
#ifdef _WIN32
        SetThreadDescription(GetCurrentThread(), L"ohao rpc reader");   // serve_bench finds it by name
#endif
        LineReader lines(transport, options.maxMessage);
        std::string line;
        for (;;) {
            const LineReader::Status st = lines.next(line);
            if (st == LineReader::Status::End) break;
            if (st == LineReader::Status::Oversize) {
                enqueueReply(errorLine("null", -32600,
                                       "the message is longer than the limit (" + std::to_string(options.maxMessage) +
                                           " bytes)"));
                continue;
            }
            if (line.find_first_not_of(" \t") == std::string::npos) continue;   // a blank line
            readLine(line);
        }
        {
            std::lock_guard lock(queueMutex);
            eof = true;
            queue.clear();               // nobody is left to answer
            cancelRunning = true;        // the running request stops at its next whole step
        }
        queueReady.notify_all();
        readerDone = true;
    }

    void readLine(const std::string& line) {
        auto parsed = ecs::parseJson(line);
        if (!parsed) {
            enqueueReply(errorLine("null", -32700, "parse error at column " + std::to_string(parsed.error().offset + 1) +
                                                       ": " + parsed.error().message));
            return;
        }
        auto doc = std::make_shared<const JsonDocument>(std::move(parsed.value()));
        const JsonNode& root = doc->root();
        if (root.type == JsonType::Array) {
            enqueueReply(errorLine("null", -32600, "batches are not supported; send one message per line"));
            return;
        }
        if (root.type != JsonType::Object) {
            enqueueReply(errorLine("null", -32600, "a message is a JSON object"));
            return;
        }
        std::string id;
        const JsonNode* idNode = doc->member(root, "id");
        if (idNode && !rawToken(*doc, *idNode, id)) {
            enqueueReply(errorLine("null", -32600, "\"id\" is a string, a number or null"));
            return;
        }
        const JsonNode* version = doc->member(root, "jsonrpc");
        if (!version || version->type != JsonType::String || version->text != "2.0") {
            if (idNode) enqueueReply(errorLine(id, -32600, "\"jsonrpc\" must be \"2.0\""));
            return;
        }
        const JsonNode* method = doc->member(root, "method");
        if (!method || method->type != JsonType::String) {
            // A response to a request we never send, or garbage: only requests get an answer.
            if (idNode && !doc->member(root, "result") && !doc->member(root, "error"))
                enqueueReply(errorLine(id, -32600, "a request needs a string \"method\""));
            return;
        }
        const JsonNode* params = doc->member(root, "params");
        if (!idNode) {
            if (method->text == "notifications/cancelled") {
                cancel(*doc, params);
                return;
            }
            auto item = std::make_shared<Item>();
            item->kind = Item::Kind::Notification;
            item->doc = doc;
            item->params = params;
            item->method.assign(method->text);
            enqueue(std::move(item));
            return;
        }
        if (method->text == "ping") {   // answered here, even while the engine is busy
            send(resultLine(id, "{}"));
            return;
        }
        auto item = std::make_shared<Item>();
        item->kind = Item::Kind::Request;
        item->doc = doc;
        item->params = params;
        item->method.assign(method->text);
        item->id = std::move(id);
        if (params && params->type == JsonType::Object)
            if (const JsonNode* meta = doc->member(*params, "_meta"); meta && meta->type == JsonType::Object)
                if (const JsonNode* token = doc->member(*meta, "progressToken"))
                    if (!rawToken(*doc, *token, item->progressToken)) item->progressToken.clear();
        enqueue(std::move(item));
    }

    void cancel(const JsonDocument& doc, const JsonNode* params) {
        if (!params || params->type != JsonType::Object) return;
        const JsonNode* requestId = doc.member(*params, "requestId");
        std::string id;
        if (!requestId || !rawToken(doc, *requestId, id)) return;
        std::lock_guard lock(queueMutex);
        if (!runningId.empty() && runningId == id) cancelRunning = true;
        for (auto& item : queue)
            if (item->kind == Item::Kind::Request && item->id == id) item->cancelled = true;
    }

    // ---- engine thread ----------------------------------------------------------------

    void start() {
        if (!reader.joinable()) reader = std::thread([this] { readerLoop(); });
    }

    // Runs what is queued; waits for the first message up to `wait` (negative: forever).
    // False once the input has ended and the queue is empty.
    bool poll(std::chrono::milliseconds wait) {
        bool waited = false;
        for (;;) {
            std::shared_ptr<Item> item;
            {
                std::unique_lock lock(queueMutex);
                if (!waited) {
                    const auto ready = [&] { return !queue.empty() || eof; };
                    if (wait.count() < 0) queueReady.wait(lock, ready);
                    else queueReady.wait_for(lock, wait, ready);
                    waited = true;
                }
                if (queue.empty()) return !eof;
                item = std::move(queue.front());
                queue.pop_front();
                if (item->cancelled) continue;   // MCP: a cancelled request gets no response
                runningId = item->id;
                cancelRunning = false;
            }
            execute(*item);
            std::lock_guard lock(queueMutex);
            runningId.clear();
        }
    }

    void flush() {
        for (const std::string& line : held) send(line);
        held.clear();
    }

    void stop() {
        if (!reader.joinable()) return;
        while (!readerDone) {   // the client still holds the input open: interrupt the read
            transport.interruptRead(reader.native_handle());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        reader.join();
    }

    void run() {
        start();
        while (poll(std::chrono::milliseconds(-1))) flush();
        flush();
        stop();
    }

    void execute(const Item& item) {
        try {
            switch (item.kind) {
            case Item::Kind::Reply: reply(item.reply); return;
            case Item::Kind::Notification: notification(item); return;
            case Item::Kind::Request: request(item); return;
            }
        } catch (const std::exception& e) {
            if (item.kind == Item::Kind::Request)
                reply(errorLine(item.id, 3, std::string("internal error: ") + e.what(),
                                R"({"code":"E_INTERNAL","diagnostics":[]})"));
        }
    }

    api::CallControl control(const Item& item) {
        api::CallControl c;
        c.cancel = &cancelRunning;
        if (!item.progressToken.empty()) {
            c.progress = [this, token = item.progressToken, last = std::chrono::steady_clock::now()](
                             uint64_t done, uint64_t total) mutable {
                const auto now = std::chrono::steady_clock::now();
                if (now - last < std::chrono::milliseconds(250)) return;
                last = now;
                send(notificationLine("notifications/progress", "{\"progressToken\":" + token + ",\"progress\":" +
                                                                    std::to_string(done) + ",\"total\":" +
                                                                    std::to_string(total) + "}"));
            };
        }
        return c;
    }

    void notification(const Item& item) {
        if (item.method.rfind("notifications/", 0) == 0) return;   // initialized, roots/list_changed, ...
        const JsonDocument& doc = item.params ? *item.doc : emptyObject();
        const JsonNode& params = item.params ? *item.params : emptyObject().root();
        const api::CallControl c = control(item);
        (void)session.call(item.method, doc, params, &c);   // a notification gets no answer
    }

    void request(const Item& item) {
        const JsonDocument& doc = item.params ? *item.doc : emptyObject();
        const JsonNode& params = item.params ? *item.params : emptyObject().root();
        const std::string& m = item.method;
        if (m == "initialize") return reply(resultLine(item.id, initialize(doc, params)));
        if (m == "tools/list") return reply(resultLine(item.id, tools()));
        if (m == "tools/call") return toolsCall(item, doc, params);
        if (m == "logging/setLevel") return setLevel(item, doc, params);
        if (const auto it = extra.find(m); it != extra.end())
            return reply(resultLine(item.id, it->second(compact(doc, params))));
        const api::CallControl c = control(item);
        api::CallOutcome o = session.call(m, doc, params, &c);
        switch (o.status) {
        case api::CallStatus::Ok: reply(resultLine(item.id, o.image ? api::withImage(o.result, *o.image) : o.result)); break;
        case api::CallStatus::Cancelled: break;   // no response (MCP's rule)
        case api::CallStatus::UnknownMethod: {
            JsonWriter w;
            w.beginObject();
            w.key("code"); w.string("E_METHOD");
            if (!o.errors.front().suggestion.empty()) { w.key("suggestion"); w.string(o.errors.front().suggestion); }
            w.endObject();
            reply(errorLine(item.id, -32601, messageOf(o.errors), w.str()));
            break;
        }
        case api::CallStatus::BadParams:
            reply(errorLine(item.id, -32602, messageOf(o.errors), diagnosticsJson(o.errors)));
            break;
        case api::CallStatus::Failed: {
            JsonWriter w(256);
            w.beginObject();
            w.key("code"); w.string(o.errors.front().code);
            w.key("diagnostics"); writeDiagnostics(w, o.errors);
            w.endObject();
            reply(errorLine(item.id, api::exitClass(o.errors.front().code), messageOf(o.errors), w.str()));
            break;
        }
        }
    }

    std::string initialize(const JsonDocument& doc, const JsonNode& params) {
        std::string_view version = kMcpVersions[std::size(kMcpVersions) - 1];
        if (params.type == JsonType::Object)
            if (const JsonNode* v = doc.member(params, "protocolVersion"); v && v->type == JsonType::String)
                for (const auto known : kMcpVersions)
                    if (v->text == known) version = known;
        JsonWriter w(1024);
        w.beginObject();
        w.key("protocolVersion"); w.string(version);
        w.key("capabilities");
        w.beginObject();
        w.key("tools"); w.beginObject(); w.endObject();
        w.key("logging"); w.beginObject(); w.endObject();
        w.endObject();
        w.key("serverInfo");
        w.beginObject();
        w.key("name"); w.string("ohao");
        w.key("version"); w.string(api::kEngineVersion);
        w.endObject();
        w.key("instructions"); w.string(kInstructions);
        w.endObject();
        return w.take();
    }

    const std::string& tools() {
        if (!toolsListCache.empty()) return toolsListCache;
        JsonWriter w(1 << 15);
        w.beginObject();
        w.key("tools");
        w.beginArray();
        for (const auto& m : api::methods()) {
            w.beginObject();
            w.key("name"); w.string(toolName(m.name));
            w.key("description"); w.string(m.doc);
            w.key("inputSchema"); reflect::writeParamsSchema(w, *m.params);
            w.endObject();
        }
        w.endArray();
        w.endObject();
        toolsListCache = w.take();
        return toolsListCache;
    }

    void toolsCall(const Item& item, const JsonDocument& doc, const JsonNode& params) {
        const JsonNode* name = params.type == JsonType::Object ? doc.member(params, "name") : nullptr;
        if (!name || name->type != JsonType::String)
            return reply(errorLine(item.id, -32602, "tools/call needs the tool's \"name\""));
        const JsonNode* args = doc.member(params, "arguments");
        if (args && args->type != JsonType::Object)
            return reply(errorLine(item.id, -32602, "\"arguments\" is an object"));
        const auto tool = toolToMethod.find(name->text);
        if (tool == toolToMethod.end()) {
            std::vector<std::string_view> names;
            for (const auto& [t, m] : toolToMethod) names.push_back(t);
            JsonWriter w;
            w.beginObject();
            w.key("code"); w.string("E_METHOD");
            if (const std::string s = reflect::suggest(name->text, names); !s.empty()) { w.key("suggestion"); w.string(s); }
            w.endObject();
            return reply(errorLine(item.id, -32601, "no tool is named '" + std::string(name->text) + "'", w.str()));
        }
        const api::CallControl c = control(item);
        api::CallOutcome o = args ? session.call(tool->second, doc, *args, &c)
                                  : session.call(tool->second, emptyObject(), emptyObject().root(), &c);
        if (o.status == api::CallStatus::Cancelled) return;
        if (o.status == api::CallStatus::BadParams)
            return reply(errorLine(item.id, -32602, messageOf(o.errors), diagnosticsJson(o.errors)));
        const bool failed = o.status != api::CallStatus::Ok;
        const std::string text = failed ? api::errorObject(std::move(o.errors)) : std::move(o.result);
        JsonWriter w(text.size() + (o.image ? o.image->base64.size() : 0) + 128);
        w.beginObject();
        w.key("content");
        w.beginArray();
        w.beginObject();
        w.key("type"); w.string("text");
        w.key("text"); w.string(text);
        w.endObject();
        if (!failed && o.image) {   // render.capture inline: the image a model looks at
            w.beginObject();
            w.key("type"); w.string("image");
            w.key("data"); w.string(o.image->base64);
            w.key("mimeType"); w.string(o.image->mimeType);
            w.endObject();
        }
        w.endArray();
        if (failed) { w.key("isError"); w.boolean(true); }
        w.endObject();
        reply(resultLine(item.id, w.str()));
    }

    void setLevel(const Item& item, const JsonDocument& doc, const JsonNode& params) {
        const JsonNode* level = params.type == JsonType::Object ? doc.member(params, "level") : nullptr;
        const auto l = level && level->type == JsonType::String ? fromMcpLevel(level->text) : std::nullopt;
        if (!l)
            return reply(errorLine(item.id, -32602,
                                  "\"level\" is one of: debug, info, notice, warning, error, critical, alert, emergency"));
        if (!sink) {
            sink = std::make_shared<Sink>(*this);
            sink->level = *l;
            log::addSink(sink);
        }
        sink->level = *l;
        if (log::level() > *l) log::setLevel(*l);   // capture what the client asked for
        reply(resultLine(item.id, "{}"));
    }
};

Server::Server(api::Session& session, Transport& transport, ServerOptions options)
    : m(std::make_unique<Impl>(session, transport, options)) {}
Server::~Server() { m->stop(); }

void Server::run() { m->run(); }
void Server::start() { m->start(); }
bool Server::poll(std::chrono::milliseconds wait) { return m->poll(wait); }
void Server::flush() { m->flush(); }
void Server::stop() { m->stop(); }

void Server::addMethod(std::string name, std::function<std::string(std::string_view)> fn) {
    m->extra.emplace(std::move(name), std::move(fn));
}

const std::string& Server::toolsList() { return m->tools(); }

std::string_view Server::instructions() noexcept { return kInstructions; }

} // namespace ohao::rpc
