// ohao: the agent/automation CLI over ohao_api (sub-project 3, step 9).
//
//   ohao schema   [-o dir]
//   ohao validate <scene> [--check-assets] [--limit N]
//   ohao fmt      <scene> [--check] [--compact] [-o out]
//   ohao apply    <scene> <commands.json> [-o out] [--seed S]
//   ohao query    <scene> [--ids a,b] [--name N] [--name-contains S] [--has T,U] [--fields /T,/T/f]
//                 [--limit N] [--offset N] [--defaults]
//   ohao summary  <scene> [--depth D] [--max-children N] [--max-nodes N]
//   ohao run      <scene> [--seed S] [--steps N] [--dt S] [--script ops.jsonl]
//                 [--render out.png [--mode deferred|rt_realtime|rt_offline] [--spp N] [--frames N]
//                  [--denoise X] [--size WxH] [--post bloom,taa,ssao] [--exposure E]
//                  [--quality realtime|reference]]
//                 [--save out.scene.json]
//   ohao doctor   [--cpu-only] [--shader-dir DIR] [--spirv-dir DIR]
//                 the environment: {"result": {ok, cpuOnly, checks: [{id, ok, level, detail, fix?}]}};
//                 exit 1 when an error-level check fails (tools/ohao/doctor.hpp)
//   ohao serve    [scene] [--out DIR] [--allow-path DIR]... [--seed S] [--log-level L] [--max-message N]
//                 [--pipe NAME] [--window [WxH] [--lockstep | --realtime] [--vsync] [--hidden]
//                                [--mode deferred|rt_realtime|rt_offline] [--spp N]]
//                 the engine as a JSON-RPC 2.0 / MCP server on stdio: native methods and MCP tools
//                 over newline-delimited JSON until stdin closes (ohao/rpc). --pipe NAME serves
//                 \\.\pipe\ohao-NAME instead: one local client of the current user, until it
//                 disconnects. Headless and lockstep by default; --window is the live host (a
//                 window, 1280x720 by default, lockstep unless --realtime) that renders while an
//                 agent drives it and a human watches; --mode is what it presents (rt_offline
//                 converges to --spp samples, default 64, after each change; captures in
//                 lockstep take any mode). Paths in params are project-relative;
//                 writes go under --out (default <root>/agent_out). Exit 0 when the input ends (or
//                 the window is closed); 1 if the scene does not load or the pipe cannot be
//                 created; 3 without a window or GPU.
//   every command: [--root DIR]   the project root asset paths are relative to (default: .)
//
// Output: one JSON document on stdout, {"result": ...} or {"error": {code, message,
// data}}. `run` without a script prints {"result": {method: result, ...}} for the
// operations it ran, or stops at the first failure and prints {"method": ..., "error": ...}.
// `run --script` writes one JSON line per operation instead: {"method": ..., "result": ...}
// or {"method": ..., "error": ...}, and runs every line. Logs go to stderr.
// Exit codes: 0 ok; 1 a validation, usage or input error; 2 an asset or instantiate error;
// 3 a GPU or internal error. No Vulkan device is created unless something renders.
#include "api/session.hpp"
#include "core/log.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "ohao/doctor.hpp"
#include "ohao/render_host.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/schema.hpp"
#include "rpc/server.hpp"
#include "rpc/transport.hpp"
#include "runtime/live_host.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace ohao;
using reflect::JsonWriter;

namespace {

namespace fs = std::filesystem;

enum Exit : int { kOk = 0, kInput = 1, kAsset = 2, kInternal = 3 };

const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (ecs::registerSceneTypes(r), true);
    (void)once;
    return r;
}

// The exit class of an error code (shared with the server's JSON-RPC error codes).
int exitFor(std::string_view code) { return api::exitClass(code); }

// {"error": {...}} with one diagnostic.
std::string errorDoc(std::string_view code, const std::string& message) {
    JsonWriter w(256);
    w.beginObject();
    w.key("error");
    w.beginObject();
    w.key("code"); w.string(code);
    w.key("message"); w.string(message);
    w.key("data");
    w.beginArray();
    w.beginObject();
    w.key("code"); w.string(code);
    w.key("path"); w.string("");
    w.key("message"); w.string(message);
    w.endObject();
    w.endArray();
    w.endObject();
    w.endObject();
    return w.take();
}

// stdout carries only JSON. The engine logs to stderr, but third-party code (Jolt, NRD,
// assimp, the driver) may print, so main() keeps a private copy of the real stdout for
// the JSON and points descriptor 1 at stderr.
FILE* g_json = stdout;

void emit(const std::string& json) {
    std::fwrite(json.data(), 1, json.size(), g_json);
    std::fputc('\n', g_json);
    std::fflush(g_json);
}

void separateLogsFromJson() {
    if (FILE* f = rpc::separateStdout()) g_json = f;
}

// A session reply's exit class: 0 for a result, else from its error code. For a
// scene.validate result with errors, the class of its errors (assets only: 2).
int replyExit(const std::string& reply) {
    auto doc = ecs::parseJson(reply);
    if (!doc) return kInternal;
    const auto& d = doc.value();
    if (const auto* err = d.member(d.root(), "error")) {
        const auto* code = d.member(*err, "code");
        return code ? exitFor(code->text) : kInput;
    }
    return kOk;
}

int validateExit(const std::string& reply) {
    auto doc = ecs::parseJson(reply);
    if (!doc) return kInternal;
    const auto& d = doc.value();
    const auto* result = d.member(d.root(), "result");
    if (!result) return replyExit(reply);
    const auto* errors = d.member(*result, "errors");
    if (!errors || errors->count == 0) return kOk;
    int worst = kOk;
    bool onlyAssets = true;
    for (uint32_t i = 0; i < errors->count; ++i) {
        const auto* code = d.member(d.child(*errors, i), "code");
        const int e = code ? exitFor(code->text) : kInput;
        onlyAssets = onlyAssets && e == kAsset;
        worst = std::max(worst, e);
    }
    return onlyAssets ? kAsset : kInput;
}

// The inner value of a {"result": X} reply.
std::string_view resultOf(const std::string& reply) {
    constexpr std::string_view kPrefix = "{\"result\":";
    return std::string_view(reply).substr(kPrefix.size(), reply.size() - kPrefix.size() - 1);
}
bool isError(const std::string& reply) { return reply.rfind("{\"error\":", 0) == 0; }

std::optional<std::string> readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> splitList(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t comma = s.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? s.size() : comma;
        if (end > start) out.emplace_back(s.substr(start, end - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

// ---- arguments -------------------------------------------------------------------------

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;   // --name value (or "" for flags)
    std::string error;
};

// `flags` take no value; every other --option takes one.
Args parseArgs(int argc, char** argv, int first, std::initializer_list<std::string_view> flags,
               std::initializer_list<std::string_view> valued) {
    Args a;
    for (int i = first; i < argc; ++i) {
        const std::string_view s = argv[i];
        if (s.size() > 1 && s[0] == '-' && !(s.size() > 1 && (s[1] >= '0' && s[1] <= '9'))) {
            const std::string name(s);
            const bool flag = std::find(flags.begin(), flags.end(), s) != flags.end();
            const bool takes = std::find(valued.begin(), valued.end(), s) != valued.end();
            if (!flag && !takes) {
                a.error = "unknown option " + name;
                return a;
            }
            if (a.options.count(name)) {
                a.error = name + " is given twice";
                return a;
            }
            if (flag) {
                a.options[name] = "";
            } else {
                if (i + 1 >= argc) {
                    a.error = name + " needs a value";
                    return a;
                }
                a.options[name] = argv[++i];
            }
        } else {
            a.positional.emplace_back(s);
        }
    }
    return a;
}

bool has(const Args& a, const char* name) { return a.options.count(name) != 0; }
std::string opt(const Args& a, const char* name, std::string fallback = {}) {
    const auto it = a.options.find(name);
    return it == a.options.end() ? fallback : it->second;
}

// An integer option, or a usage error.
bool intOpt(const Args& a, const char* name, long long lo, long long hi, long long& out, std::string& error) {
    if (!has(a, name)) return true;
    const std::string v = opt(a, name);
    try {
        std::size_t used = 0;
        const long long x = std::stoll(v, &used);
        if (used != v.size() || x < lo || x > hi) throw std::out_of_range(v);
        out = x;
        return true;
    } catch (...) {
        error = std::string(name) + " needs an integer in [" + std::to_string(lo) + ", " + std::to_string(hi) +
                "], got '" + v + "'";
        return false;
    }
}

// A number option is spliced into the params as JSON, so it must be a JSON number;
// its range is the session's to check.
bool numberOpt(const Args& a, const char* name, std::string& error) {
    if (!has(a, name)) return true;
    const std::string v = opt(a, name);
    auto doc = ecs::parseJson(v);
    if (doc && doc.value().root().type == reflect::JsonType::Number) return true;
    error = std::string(name) + " needs a number, got '" + v + "'";
    return false;
}

int usage(const std::string& message) {
    emit(errorDoc("E_COMMAND", message));
    return kInput;
}

// ---- commands --------------------------------------------------------------------------

struct Context {
    std::unique_ptr<ecs::FileAssetResolver> assets;
    std::unique_ptr<host::RenderHost> render;   // before the world: it must outlive it
    std::unique_ptr<ecs::World> world;
    std::unique_ptr<api::Session> session;

    explicit Context(const std::string& root, api::HostKind kind = api::HostKind::Cli) {
        assets = std::make_unique<ecs::FileAssetResolver>(root);
        render = std::make_unique<host::RenderHost>(*assets, kind);
        world = std::make_unique<ecs::World>();
        session = std::make_unique<api::Session>(*world, engineTypes());
        session->setAssetResolver(assets.get());
        render->bind(*world);
        session->setHost(render.get());
    }
    ~Context() {
        session.reset();
        world.reset();   // releases its RenderScene slots while the renderer is alive
        render.reset();
    }
};

std::string params(const std::vector<std::pair<std::string, std::string>>& members) {
    // members: key -> already-encoded JSON value
    std::string s = "{";
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (i) s += ',';
        JsonWriter k;
        k.string(members[i].first);
        s += k.str();
        s += ':';
        s += members[i].second;
    }
    return s + "}";
}

std::string jsonString(std::string_view s) {
    JsonWriter w;
    w.string(s);
    return w.take();
}

std::string jsonStrings(const std::vector<std::string>& items) {
    JsonWriter w;
    w.beginArray();
    for (const auto& s : items) w.string(s);
    w.endArray();
    return w.take();
}

// Loads `scene` into the context's world. Empty on success, else the error reply.
std::string loadScene(Context& c, const std::string& scene, const std::string& seed = {}) {
    std::vector<std::pair<std::string, std::string>> p{{"path", jsonString(scene)}};
    if (!seed.empty()) p.emplace_back("seed", jsonString(seed));
    std::string reply = c.session->call("scene.load", params(p));
    return isError(reply) ? reply : std::string();
}

int cmdSchema(const Args& a) {
    if (!a.positional.empty()) return usage("schema takes no scene");
    const fs::path dir = opt(a, "-o", ".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto catalogue = reflect::exportCatalogue(engineTypes(), {api::methods(), reflect::JsonLayout::Pretty});
    const auto schema = reflect::exportSceneJsonSchema(engineTypes());
    const std::string files[] = {(dir / "ohao_schema.json").generic_string(), (dir / "scene.schema.json").generic_string()};
    for (int i = 0; i < 2; ++i) {
        std::ofstream f(files[i], std::ios::binary | std::ios::trunc);
        f << (i == 0 ? catalogue : schema);
        if (!f) {
            emit(errorDoc("E_IO", "cannot write " + files[i]));
            return kInput;
        }
    }
    JsonWriter w;
    w.beginObject();
    w.key("result");
    w.beginObject();
    w.key("files");
    w.beginArray();
    for (const auto& f : files) w.string(f);
    w.endArray();
    w.key("schemaHash"); w.hex64(reflect::schemaHash(engineTypes(), api::methods()));
    w.endObject();
    w.endObject();
    emit(w.take());
    return kOk;
}

int cmdValidate(const Args& a) {
    if (a.positional.size() != 1) return usage("validate needs one scene file");
    Context c(opt(a, "--root", "."));
    long long limit = 20;
    std::string err;
    if (!intOpt(a, "--limit", 0, 100, limit, err)) return usage(err);
    const std::string reply = c.session->call(
        "scene.validate", params({{"path", jsonString(a.positional[0])},
                                  {"checkAssets", has(a, "--check-assets") ? "true" : "false"},
                                  {"limit", std::to_string(limit)}}));
    emit(reply);
    return validateExit(reply);
}

int cmdFmt(const Args& a) {
    if (a.positional.size() != 1) return usage("fmt needs one scene file");
    const std::string& path = a.positional[0];
    const auto text = readText(path);
    if (!text) {
        emit(errorDoc("E_IO", "cannot read " + path));
        return kInput;
    }
    ecs::World world;
    auto loaded = ecs::loadScene(world, *text, engineTypes());
    if (!loaded) {
        std::string reply = "{\"error\":{\"code\":" + jsonString(loaded.error().front().code) + ",\"message\":" +
                            jsonString(loaded.error().front().message) + ",\"data\":" +
                            ecs::diagnosticsJson(loaded.error()) + "}}";
        emit(reply);
        return kInput;
    }
    ecs::SaveOptions o;
    o.layout = has(a, "--compact") ? reflect::JsonLayout::Compact : reflect::JsonLayout::Pretty;
    const auto canonical = ecs::saveScene(world, engineTypes(), o);
    if (!canonical) {
        emit(errorDoc(canonical.error().code, canonical.error().message));
        return kInput;
    }
    const bool same = canonical.value() == *text;
    JsonWriter w;
    w.beginObject();
    w.key("result");
    w.beginObject();
    if (has(a, "--check")) {
        w.key("path"); w.string(path);
        w.key("canonical"); w.boolean(same);
        w.endObject();
        w.endObject();
        emit(w.take());
        return same ? kOk : kInput;
    }
    const std::string out = opt(a, "-o", path);
    if (!same || out != path) {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << canonical.value();
        if (!f) {
            emit(errorDoc("E_IO", "cannot write " + out));
            return kInput;
        }
    }
    w.key("path"); w.string(out);
    w.key("changed"); w.boolean(!same);
    w.key("bytes"); w.uint64(canonical.value().size());
    w.endObject();
    w.endObject();
    emit(w.take());
    return kOk;
}

int cmdApply(const Args& a) {
    if (a.positional.size() != 2) return usage("apply needs a scene file and a commands file");
    Context c(opt(a, "--root", "."));
    // --seed fixes the ids the commands create (as in run); without it they are random.
    if (std::string err = loadScene(c, a.positional[0], opt(a, "--seed")); !err.empty()) {
        emit(err);
        return replyExit(err);
    }
    const auto commands = readText(a.positional[1]);
    if (!commands) {
        emit(errorDoc("E_IO", "cannot read " + a.positional[1]));
        return kInput;
    }
    // The file is the commands array, or a world.apply params object {"commands": [...]}.
    std::size_t first = commands->find_first_not_of(" \t\r\n");
    const bool isArray = first != std::string::npos && (*commands)[first] == '[';
    const std::string applied =
        c.session->call("world.apply", isArray ? "{\"commands\":" + *commands + "}" : *commands);
    if (isError(applied)) {
        emit(applied);
        return replyExit(applied);
    }
    const std::string out = opt(a, "-o", a.positional[0]);
    const std::string saved =
        c.session->call("scene.save", params({{"path", jsonString(out)}, {"layout", "\"pretty\""}}));
    if (isError(saved)) {
        emit(saved);
        return replyExit(saved);
    }
    emit("{\"result\":{\"apply\":" + std::string(resultOf(applied)) + ",\"save\":" + std::string(resultOf(saved)) +
         "}}");
    return kOk;
}

int cmdQuery(const Args& a) {
    if (a.positional.size() != 1) return usage("query needs one scene file");
    Context c(opt(a, "--root", "."));
    std::vector<std::pair<std::string, std::string>> p;
    if (has(a, "--ids")) p.emplace_back("ids", jsonStrings(splitList(opt(a, "--ids"))));
    if (has(a, "--name")) p.emplace_back("names", jsonStrings({opt(a, "--name")}));
    if (has(a, "--name-contains")) p.emplace_back("nameContains", jsonString(opt(a, "--name-contains")));
    if (has(a, "--has")) p.emplace_back("has", jsonStrings(splitList(opt(a, "--has"))));
    if (has(a, "--fields")) {
        // "Type/field" is accepted for "/Type/field": Git Bash (MSYS) rewrites an argument
        // that starts with '/' into a Windows path before the program sees it.
        auto fields = splitList(opt(a, "--fields"));
        for (auto& f : fields)
            if (!f.empty() && f[0] != '/') f.insert(f.begin(), '/');
        p.emplace_back("fields", jsonStrings(fields));
    }
    long long limit = 50, offset = 0;
    std::string err;
    if (!intOpt(a, "--limit", 0, 1'000'000, limit, err) || !intOpt(a, "--offset", 0, 1'000'000'000, offset, err))
        return usage(err);
    if (has(a, "--limit")) p.emplace_back("limit", std::to_string(limit));
    if (has(a, "--offset")) p.emplace_back("offset", std::to_string(offset));
    if (has(a, "--defaults")) p.emplace_back("defaults", "true");
    if (std::string e = loadScene(c, a.positional[0]); !e.empty()) {
        emit(e);
        return replyExit(e);
    }
    const std::string reply = c.session->call("world.query", params(p));
    emit(reply);
    return replyExit(reply);
}

int cmdSummary(const Args& a) {
    if (a.positional.size() != 1) return usage("summary needs one scene file");
    Context c(opt(a, "--root", "."));
    std::vector<std::pair<std::string, std::string>> p;
    std::string err;
    for (const auto& [flag, key] : {std::pair{"--depth", "depth"}, std::pair{"--max-children", "maxChildren"},
                                    std::pair{"--max-nodes", "maxNodes"}}) {
        long long v = 0;
        if (!intOpt(a, flag, 0, 1'000'000, v, err)) return usage(err);
        if (has(a, flag)) p.emplace_back(key, std::to_string(v));
    }
    if (std::string e = loadScene(c, a.positional[0]); !e.empty()) {
        emit(e);
        return replyExit(e);
    }
    const std::string reply = c.session->call("world.summary", params(p));
    emit(reply);
    return replyExit(reply);
}

int cmdRun(const Args& a) {
    if (a.positional.size() != 1) return usage("run needs one scene file");
    // Everything is checked before the scene loads.
    long long steps = 0, spp = 16, frames = 0;
    std::string err;
    if (!intOpt(a, "--steps", 0, 1'000'000, steps, err) || !intOpt(a, "--spp", 1, 65536, spp, err) ||
        !intOpt(a, "--frames", 1, 100000, frames, err) || !numberOpt(a, "--dt", err) ||
        !numberOpt(a, "--exposure", err))
        return usage(err);
    if (has(a, "--dt") && !has(a, "--steps")) return usage("--dt needs --steps");
    const bool render = has(a, "--render");
    for (const char* o : {"--mode", "--spp", "--frames", "--denoise", "--size", "--post", "--exposure", "--quality"})
        if (has(a, o) && !render) return usage(std::string(o) + " needs --render");
    std::string width = "1920", height = "1080";
    if (has(a, "--size")) {
        const std::string s = opt(a, "--size");
        const std::size_t x = s.find('x');
        if (x == std::string::npos || x == 0 || x + 1 >= s.size() ||
            s.find_first_not_of("0123456789", 0) != x || s.find_first_not_of("0123456789", x + 1) != std::string::npos)
            return usage("--size needs WIDTHxHEIGHT, got '" + s + "'");
        width = s.substr(0, x);
        height = s.substr(x + 1);
    }
    std::optional<std::string> script;
    if (has(a, "--script")) {
        script = readText(opt(a, "--script"));
        if (!script) {
            emit(errorDoc("E_IO", "cannot read " + opt(a, "--script")));
            return kInput;
        }
    }

    Context c(opt(a, "--root", "."));
    const bool lines = script.has_value();
    int worst = kOk;
    std::string combined;   // without a script: {"result": {method: result, ...}}
    // One operation: its reply printed as a line (script mode) or collected. False on an
    // error outside script mode (the run stops there).
    const auto step = [&](const std::string& method, const std::string& reply) {
        const int e = replyExit(reply);
        worst = std::max(worst, e);
        const bool error = isError(reply);
        const std::string body = std::string(reply).substr(1, reply.size() - 2);   // "result":X or "error":X
        if (lines) {
            emit("{\"method\":" + jsonString(method) + "," + body + "}");
            return true;
        }
        if (error) {
            emit("{\"method\":" + jsonString(method) + "," + body + "}");
            return false;
        }
        combined += (combined.empty() ? "" : ",") + jsonString(method) + ":" + std::string(resultOf(reply));
        return true;
    };

    std::vector<std::pair<std::string, std::string>> load{{"path", jsonString(a.positional[0])}};
    if (has(a, "--seed")) load.emplace_back("seed", jsonString(opt(a, "--seed")));
    const std::string loaded = c.session->call("scene.load", params(load));
    if (!step("scene.load", loaded) || isError(loaded)) return worst;

    if (has(a, "--steps") && steps > 0) {
        std::vector<std::pair<std::string, std::string>> p{{"frames", std::to_string(steps)}};
        if (has(a, "--dt")) p.emplace_back("dt", opt(a, "--dt"));
        if (!step("world.step", c.session->call("world.step", params(p)))) return worst;
    }
    if (script) {
        std::istringstream in(*script);
        std::string line;
        uint32_t number = 0;
        while (std::getline(in, line)) {
            ++number;
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            auto doc = ecs::parseJson(line);
            const reflect::JsonNode* method = nullptr;
            const reflect::JsonNode* p = nullptr;
            if (doc && doc.value().root().type == reflect::JsonType::Object) {
                method = doc.value().member(doc.value().root(), "method");
                p = doc.value().member(doc.value().root(), "params");
            }
            if (!method || method->type != reflect::JsonType::String) {
                const std::string e = errorDoc("E_COMMAND", "script line " + std::to_string(number) +
                                                                " is not {\"method\": \"...\", \"params\": {...}}");
                step("", e);
                continue;
            }
            std::string ptext = "{}";
            if (p) {
                JsonWriter w;
                reflect::writeJson(w, doc.value(), *p);
                ptext = w.take();
            }
            step(std::string(method->text), c.session->call(method->text, ptext));
        }
    }
    if (render) {
        std::vector<std::pair<std::string, std::string>> p{{"path", jsonString(opt(a, "--render"))},
                                                           {"width", width},
                                                           {"height", height}};
        if (has(a, "--mode")) p.emplace_back("mode", jsonString(opt(a, "--mode")));
        if (has(a, "--spp")) p.emplace_back("spp", std::to_string(spp));
        if (has(a, "--frames")) p.emplace_back("frames", std::to_string(frames));
        if (has(a, "--denoise")) p.emplace_back("denoise", jsonString(opt(a, "--denoise")));
        if (has(a, "--post")) p.emplace_back("post", jsonStrings(splitList(opt(a, "--post"))));
        if (has(a, "--exposure")) p.emplace_back("exposure", opt(a, "--exposure"));
        if (has(a, "--quality")) p.emplace_back("quality", jsonString(opt(a, "--quality")));
        if (!step("render.capture", c.session->call("render.capture", params(p)))) return worst;
    }
    if (has(a, "--save")) {
        const std::string reply =
            c.session->call("scene.save", params({{"path", jsonString(opt(a, "--save"))}, {"layout", "\"pretty\""}}));
        if (!step("scene.save", reply)) return worst;
    }
    if (!lines) emit("{\"result\":{" + combined + "}}");
    return worst;
}

int cmdDoctor(const Args& a) {
    if (!a.positional.empty()) return usage("doctor takes no scene");
    doctor::Options o;
    o.cpuOnly = has(a, "--cpu-only");
    o.root = opt(a, "--root", ".");
    if (has(a, "--shader-dir")) o.shaderDir = opt(a, "--shader-dir");
    if (has(a, "--spirv-dir")) o.spirvDir = opt(a, "--spirv-dir");
    const auto checks = doctor::run(o);
    emit("{\"result\":" + doctor::toJson(checks, o.cpuOnly) + "}");
    return doctor::passed(checks) ? kOk : kInput;
}

// --window [WxH]: absent, or the window's size (default 1280x720).
struct WindowOption {
    bool on{false};
    uint32_t width{1280};
    uint32_t height{720};
};

// The engine as a server over stdio until stdin closes (or over --pipe NAME until its one
// client disconnects): headless (lockstep), or with --window the live host
// (runtime::LiveHost), which serves requests once per frame and renders and presents while
// an agent drives it. Nothing is printed on stdout but protocol messages; a scene that does
// not load is reported on stderr before serving.
int serveWindow(const Args& a, const std::vector<std::string>& allowPaths, const WindowOption& win,
                std::size_t maxMessage, rpc::Transport& transport) {
    runtime::LiveConfig cfg;
    cfg.window.width = win.width;
    cfg.window.height = win.height;
    cfg.window.title = "OHAO serve";
    cfg.window.visible = !has(a, "--hidden");
    cfg.vsync = has(a, "--vsync");
    cfg.timeMode = has(a, "--realtime") ? TimeMode::realtime : TimeMode::lockstep;
    cfg.root = opt(a, "--root", ".");
    const std::string mode = opt(a, "--mode", "deferred");
    cfg.mode = mode == "rt_realtime" ? RenderMode::RTRealtime : mode == "rt_offline" ? RenderMode::RTOffline : RenderMode::Deferred;
    long long spp = 64;
    std::string err;
    if (intOpt(a, "--spp", 1, 65536, spp, err)) cfg.spp = static_cast<int>(spp);
    runtime::LiveHost host;
    if (!host.initialize(cfg, engineTypes())) {
        OHAO_LOG(Error, rpc, "serve: no window or no usable GPU");
        return kInternal;
    }
    api::PathPolicy policy;
    policy.root = cfg.root;
    policy.out = opt(a, "--out");
    for (const auto& p : allowPaths) policy.allowed.emplace_back(p);
    host.session().setPathPolicy(std::move(policy));
    if (!a.positional.empty()) {
        std::vector<std::pair<std::string, std::string>> p{{"path", jsonString(a.positional[0])}};
        if (has(a, "--seed")) p.emplace_back("seed", jsonString(opt(a, "--seed")));
        const std::string reply = host.session().call("scene.load", params(p));
        if (isError(reply)) {
            OHAO_LOG(Error, rpc, "serve: the scene did not load: {}", reply);
            (void)host.shutdown();
            return replyExit(reply);
        }
    }
    rpc::ServerOptions options;
    options.maxMessage = maxMessage;
    options.holdReplies = true;   // sent after the frame the requests caused is presented
    rpc::Server server(host.session(), transport, options);
    runtime::RequestPump pump;
    pump.poll = [&server](std::chrono::milliseconds wait) { return server.poll(wait); };
    pump.flush = [&server] { server.flush(); };
    server.start();
    (void)host.run(runtime::GameHooks{}, &pump);
    server.stop();   // the window closed while the client still holds stdin: stop reading
    (void)host.shutdown();
    return kOk;
}

int cmdServe(const Args& a, const std::vector<std::string>& allowPaths, const WindowOption& win) {
    if (a.positional.size() > 1) return usage("serve takes at most one scene");
    long long maxMessage = 64ll << 20;
    std::string err;
    if (!intOpt(a, "--max-message", 1024, 1ll << 30, maxMessage, err)) return usage(err);
    if (has(a, "--seed") && a.positional.empty()) return usage("--seed needs a scene");
    if (has(a, "--lockstep") && has(a, "--realtime")) return usage("--lockstep and --realtime exclude each other");
    for (const char* o : {"--realtime", "--vsync", "--hidden", "--mode", "--spp"})
        if (has(a, o) && !win.on)
            return usage(std::string(o) + " needs --window (a headless host is lockstep; its captures name their mode)");
    if (has(a, "--mode")) {
        const std::string m = opt(a, "--mode");
        if (m != "deferred" && m != "rt_realtime" && m != "rt_offline")
            return usage("--mode is one of: deferred, rt_realtime, rt_offline");
    }
    long long spp = 64;
    if (!intOpt(a, "--spp", 1, 65536, spp, err)) return usage(err);
    if (has(a, "--log-level")) {
        const auto level = log::parseLevel(opt(a, "--log-level"));
        if (!level) return usage("--log-level is one of: trace, debug, info, warn, error, off");
        log::setLevel(*level);
        log::stderrSink()->level = *level == log::Level::Off ? log::Level::Error : *level;
        if (*level == log::Level::Off) log::removeSink(log::stderrSink().get());
    }
    // The byte stream: stdio, or the named pipe, created before anything else so a bad or
    // taken name fails at once.
    std::unique_ptr<rpc::Transport> transport;
    if (has(a, "--pipe")) {
        std::string pipeError;
        auto pipe = rpc::PipeTransport::create(opt(a, "--pipe"), pipeError);
        if (!pipe) return usage("--pipe: " + pipeError);
        OHAO_LOG(Info, rpc, "serving on {}", pipe->path());
        transport = std::move(pipe);
    } else {
        transport = std::make_unique<rpc::StdioTransport>(g_json);
    }
    if (win.on) return serveWindow(a, allowPaths, win, static_cast<std::size_t>(maxMessage), *transport);
    const std::string root = opt(a, "--root", ".");
    Context c(root, api::HostKind::Headless);
    api::PathPolicy policy;
    policy.root = root;
    policy.out = opt(a, "--out");
    for (const auto& p : allowPaths) policy.allowed.emplace_back(p);
    c.session->setPathPolicy(std::move(policy));
    if (!a.positional.empty()) {
        if (std::string e = loadScene(c, a.positional[0], opt(a, "--seed")); !e.empty()) {
            OHAO_LOG(Error, rpc, "serve: the scene did not load: {}", e);
            return replyExit(e);
        }
    }
    rpc::ServerOptions options;
    options.maxMessage = static_cast<std::size_t>(maxMessage);
    rpc::Server server(*c.session, *transport, options);
    server.run();
    return kOk;
}

int run(int argc, char** argv) {
    if (argc < 2)
        return usage(
            "usage: ohao <schema|validate|fmt|apply|query|summary|run|doctor|serve> ... (see tools/ohao/main.cpp)");
    const std::string_view cmd = argv[1];
    if (cmd == "serve") {
        // --allow-path may be given more than once and --window takes an optional WxH; the
        // rest parses as usual.
        std::vector<std::string> allow;
        WindowOption win;
        std::vector<char*> rest{argv[0], argv[1]};
        for (int i = 2; i < argc; ++i) {
            const std::string_view s = argv[i];
            if (s == "--allow-path") {
                if (i + 1 >= argc) return usage("--allow-path needs a value");
                allow.emplace_back(argv[++i]);
            } else if (s == "--window") {
                if (win.on) return usage("--window is given twice");
                win.on = true;
                if (i + 1 < argc) {   // an optional WxH
                    const std::string_view v = argv[i + 1];
                    const std::size_t x = v.find('x');
                    unsigned w = 0, h = 0;
                    if (x != std::string_view::npos && x > 0 &&
                        std::from_chars(v.data(), v.data() + x, w).ptr == v.data() + x &&
                        std::from_chars(v.data() + x + 1, v.data() + v.size(), h).ptr == v.data() + v.size()) {
                        if (w < 64 || h < 64 || w > 8192 || h > 8192)
                            return usage("--window WxH needs sizes in [64, 8192], got '" + std::string(v) + "'");
                        win.width = w;
                        win.height = h;
                        ++i;
                    }
                }
            } else {
                rest.push_back(argv[i]);
            }
        }
        const Args a = parseArgs(static_cast<int>(rest.size()), rest.data(), 2,
                                 {"--lockstep", "--realtime", "--vsync", "--hidden"},
                                 {"--root", "--out", "--seed", "--log-level", "--max-message", "--pipe", "--mode", "--spp"});
        return a.error.empty() ? cmdServe(a, allow, win) : usage(a.error);
    }
    if (cmd == "schema") {
        const Args a = parseArgs(argc, argv, 2, {}, {"-o"});
        return a.error.empty() ? cmdSchema(a) : usage(a.error);
    }
    if (cmd == "validate") {
        const Args a = parseArgs(argc, argv, 2, {"--check-assets"}, {"--limit", "--root"});
        return a.error.empty() ? cmdValidate(a) : usage(a.error);
    }
    if (cmd == "fmt") {
        const Args a = parseArgs(argc, argv, 2, {"--check", "--compact"}, {"-o"});
        return a.error.empty() ? cmdFmt(a) : usage(a.error);
    }
    if (cmd == "apply") {
        const Args a = parseArgs(argc, argv, 2, {}, {"-o", "--root", "--seed"});
        return a.error.empty() ? cmdApply(a) : usage(a.error);
    }
    if (cmd == "query") {
        const Args a = parseArgs(argc, argv, 2, {"--defaults"},
                                 {"--ids", "--name", "--name-contains", "--has", "--fields", "--limit", "--offset", "--root"});
        return a.error.empty() ? cmdQuery(a) : usage(a.error);
    }
    if (cmd == "summary") {
        const Args a = parseArgs(argc, argv, 2, {}, {"--depth", "--max-children", "--max-nodes", "--root"});
        return a.error.empty() ? cmdSummary(a) : usage(a.error);
    }
    if (cmd == "run") {
        const Args a = parseArgs(argc, argv, 2, {},
                                 {"--seed", "--steps", "--dt", "--script", "--render", "--mode", "--spp", "--frames",
                                  "--denoise", "--size", "--post", "--exposure", "--quality", "--save", "--root"});
        return a.error.empty() ? cmdRun(a) : usage(a.error);
    }
    if (cmd == "doctor") {
        const Args a = parseArgs(argc, argv, 2, {"--cpu-only"}, {"--root", "--shader-dir", "--spirv-dir"});
        return a.error.empty() ? cmdDoctor(a) : usage(a.error);
    }
    return usage("unknown command '" + std::string(cmd) +
                 "'; one of: schema, validate, fmt, apply, query, summary, run, doctor, serve");
}

} // namespace

int main(int argc, char** argv) {
    separateLogsFromJson();
    try {
        const int code = run(argc, argv);
        std::fflush(g_json);
        return code;
    } catch (const std::exception& e) {
        emit(errorDoc("E_INTERNAL", std::string("internal error: ") + e.what()));
        return kInternal;
    } catch (...) {
        emit(errorDoc("E_INTERNAL", "internal error"));
        return kInternal;
    }
}
