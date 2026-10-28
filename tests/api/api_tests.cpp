// API tests (sub-project 3, step 5: test 9 plus the token-budget checks). CPU only.
//
//   api_tests     run every test; last line is a JSON summary
//
// Everything goes through Session::call with JSON text, exactly as the CLI and the future
// RPC server will.
#include "api/params.hpp"
#include "api/session.hpp"
#include "core/log.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/animation.hpp"
#include "ecs/audio_schedule.hpp"
#include "ecs/combat.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "level/level_system.hpp"
#include "ecs/json_parse.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/world.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/schema.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
static int processId() { return _getpid(); }
#else
#include <unistd.h>
static int processId() { return static_cast<int>(getpid()); }
#endif

namespace fs = std::filesystem;
using namespace ohao;
using namespace ohao::ecs;
using reflect::JsonDocument;
using reflect::JsonNode;
using reflect::JsonType;
using reflect::JsonWriter;

// ---- harness ------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;
static std::vector<std::string> g_failures;
static const char* g_current = "";
#define CHECK(expr, msg)                                                                     \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            std::printf("    FAIL: %s  [%s]\n", msg, #expr);                                 \
            ++g_fail;                                                                        \
            g_failures.push_back(std::string(g_current) + ": " + (msg));                     \
            return;                                                                          \
        }                                                                                    \
    } while (0)
#define RUN(fn)                                                       \
    do {                                                              \
        g_current = #fn;                                              \
        std::printf("  %-56s", #fn);                                  \
        int f0 = g_fail;                                              \
        fn();                                                         \
        if (g_fail == f0) { ++g_pass; std::printf("PASS\n"); }        \
    } while (0)

static const fs::path kScenes = fs::path(OHAO_SOURCE_DIR) / "tests" / "scenes";

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static const reflect::TypeRegistry& engineTypes() {
    static reflect::TypeRegistry r;
    static const bool once = (registerSceneTypes(r), true);
    (void)once;
    return r;
}

static std::string save(const World& w, reflect::JsonLayout layout = reflect::JsonLayout::Pretty) {
    auto s = saveScene(w, engineTypes(), SaveOptions{layout});
    return s ? s.value() : std::string("<save failed>");
}

// A parsed reply with JSON Pointer access.
struct Reply {
    std::string text;
    JsonDocument doc;

    explicit Reply(std::string t) : text(std::move(t)) {
        auto r = parseJson(text);
        if (r) doc = std::move(r.value());
        else std::printf("\n      reply is not JSON: %s", text.c_str());
    }
    [[nodiscard]] bool ok() const { return text.rfind("{\"result\":", 0) == 0; }
    [[nodiscard]] const JsonNode* at(std::string_view ptr) const {
        if (doc.nodes.empty()) return nullptr;
        const JsonNode* n = &doc.root();
        while (!ptr.empty()) {
            ptr.remove_prefix(1);
            const std::size_t slash = ptr.find('/');
            const std::string_view seg = ptr.substr(0, slash);
            ptr = slash == std::string_view::npos ? std::string_view{} : ptr.substr(slash);
            if (n->type == JsonType::Object) {
                n = doc.member(*n, seg);
            } else if (n->type == JsonType::Array) {
                const auto i = static_cast<uint32_t>(std::stoul(std::string(seg)));
                n = i < n->count ? &doc.child(*n, i) : nullptr;
            } else {
                n = nullptr;
            }
            if (!n) return nullptr;
        }
        return n;
    }
    // A string or number member's text ("" when absent).
    [[nodiscard]] std::string str(std::string_view ptr) const {
        const JsonNode* n = at(ptr);
        return n && (n->type == JsonType::String || n->type == JsonType::Number) ? std::string(n->text) : "";
    }
    // Any member, as compact JSON ("" when absent).
    [[nodiscard]] std::string json(std::string_view ptr) const {
        const JsonNode* n = at(ptr);
        if (!n) return "";
        JsonWriter w;
        reflect::writeJson(w, doc, *n);
        return w.take();
    }
    [[nodiscard]] std::string code() const { return str("/error/code"); }
};

static Reply applyBatch(api::Session& s, const std::string& commands) {
    return Reply(s.call("world.apply", "{\"commands\":" + commands + "}"));
}

static Reply undo(api::Session& s, const Reply& applied) { return applyBatch(s, applied.json("/result/inverse")); }

static void loadFile(World& w, const char* file) {
    auto r = loadScene(w, readFile(kScenes / "v1" / file), engineTypes());
    if (!r) std::printf("\n      load %s failed", file);
}

// rooms.scene.json: Room{Lamp{Shade}}, Sun. Lamp and Sun have lights.
constexpr const char* kRoom = "1000000000000001";
constexpr const char* kLamp = "1000000000000002";
constexpr const char* kSun = "1000000000000003";
constexpr const char* kShade = "1000000000000004";

static entt::entity byHex(const World& w, const char* hex) { return w.findById(std::stoull(hex, nullptr, 16)); }

// ---- t09: method table ------------------------------------------------------------------

static void t09_method_table_dispatch_and_errors() {
    World w;
    api::Session s(w, engineTypes());
    const Reply info(s.call("world.info"));
    CHECK(info.ok() && info.str("/result/entities") == "0" && info.str("/result/schemaHash").size() == 16,
          "world.info answers with no params");
    const Reply bad(s.call("world.inof", "{}"));
    CHECK(bad.code() == "E_METHOD" && bad.str("/error/data/0/suggestion") == "world.info",
          "an unknown method is E_METHOD with a suggestion");
    CHECK(Reply(s.call("world.info", "[1]")).code() == "E_TYPE", "params must be an object");
    CHECK(Reply(s.call("world.info", "{bad")).code() == "E_PARSE", "params must be JSON");
    const Reply typo(s.call("world.query", R"({"limt":5})"));
    CHECK(typo.code() == "E_UNKNOWN_FIELD" && typo.str("/error/data/0/path") == "/limt" &&
              typo.str("/error/data/0/suggestion") == "limit",
          "an unknown param is E_UNKNOWN_FIELD at its path, with a suggestion");
    CHECK(api::methods().size() == 38, "thirty-eight methods");
    for (const auto& m : api::methods()) {
        const Reply r(s.call(m.name, "{}"));
        CHECK(r.code() != "E_METHOD", "every documented method dispatches");
        CHECK(m.params && !m.result.empty() && !m.doc.empty(), "and is documented, with a params struct");
    }
}

static void t09_schema_get_includes_methods() {
    World w;
    api::Session s(w, engineTypes());
    const Reply r(s.call("schema.get"));
    CHECK(r.ok(), "schema.get answers");
    const JsonNode* methods = r.at("/result/methods");
    CHECK(methods && methods->count == api::methods().size(), "the catalogue lists every method");
    for (uint32_t i = 0; i < methods->count; ++i)
        CHECK(r.str("/result/methods/" + std::to_string(i) + "/name") == api::methods()[i].name, "in table order");
    CHECK(r.str("/result/schemaHash") == Reply(s.call("world.info")).str("/result/schemaHash"),
          "the catalogue's schemaHash is the session's");
    CHECK(r.text.find(R"("name":"Transform","doc")") != std::string::npos &&
              r.text.find(R"("alwaysPresent":true)") != std::string::npos,
          "Name and Transform are marked alwaysPresent");
    CHECK(r.text.find("E_PATH") != std::string::npos && r.text.find("E_COMPONENT_UNKNOWN") != std::string::npos,
          "and the new error codes are in the table");
}

// A host that records the requests it gets and answers with `fn`.
struct FakeHost final : api::Host {
    std::function<api::RenderOutcome(const api::RenderRequest&)> fn;
    api::HostKind hostKind{api::HostKind::Headless};
    [[nodiscard]] api::HostKind kind() const noexcept override { return hostKind; }
    [[nodiscard]] api::RenderOutcome capture(const api::RenderRequest& r) override { return fn(r); }
    std::optional<api::RenderStats> stats;   // render.stats' answer; nullopt: nothing rendered yet
    std::vector<int> statsAsked;
    [[nodiscard]] std::optional<api::RenderStats> renderStats(int frames) override {
        statsAsked.push_back(frames);
        return stats;
    }
    std::optional<api::RenderSettings> settings;   // render.settings' state; nullopt: none (E_STATE)
    int settingsSet{0};
    [[nodiscard]] std::optional<api::RenderSettings> renderSettings() override { return settings; }
    void setRenderSettings(const api::RenderSettings& s) override {
        settings = s;
        ++settingsSet;
    }
};

// render.settings (2b test 14): E_STATE on a host without settings (the CLI), reads the
// host's settings, a set changes only the members given (nested ones too), limits and
// unknown fields are refused before the host is touched, a set counts as a change and a read
// does not; captures take quality, post (ssao) and exposure from the settings unless they
// name their own, and carry gpuMs and (timings) the pass list.
static void sp2b_t14_render_settings_reads_changes_and_defaults_captures() {
    World w;
    api::Session s(w, engineTypes());
    CHECK(Reply(s.call("render.settings")).code() == "E_STATE", "without a host: E_STATE");
    FakeHost host;
    std::vector<api::RenderRequest> seen;
    host.fn = [&](const api::RenderRequest& r) {
        seen.push_back(r);
        api::RenderOutcome o;
        o.width = 4;
        o.height = 4;
        o.gpuMs = 1.23456;
        if (r.timings) o.timings = {{"gbuffer.early", 0.0712843}, {"lighting", 0.27}};
        return o;
    };
    s.setHost(&host);
    CHECK(Reply(s.call("render.settings")).code() == "E_STATE", "a host without settings (the CLI): E_STATE");

    host.settings = api::RenderSettings{};
    uint64_t changes = s.changeCount();
    const Reply read(s.call("render.settings"));
    CHECK(read.ok() && read.json("/result/quality") == R"("realtime")" && read.json("/result/rtGI/rays") == "2" &&
              read.json("/result/rtGI/resolution") == R"("half")" && read.json("/result/geometry") == R"("auto")" &&
              read.json("/result/csm/size") == "2048" && read.json("/result/ssr") == "true",
          "a read returns every setting, the budgeted defaults");
    CHECK(s.changeCount() == changes && host.settingsSet == 0, "a read changes nothing");

    const Reply set(s.call("render.settings", R"({"set":{"rtGI":{"rays":8},"ssr":false,"quality":"reference"}})"));
    CHECK(set.ok() && host.settingsSet == 1 && host.settings->rtGI.rays == 8 && host.settings->rtGI.enabled &&
              host.settings->rtGI.resolution == api::GIResolution::half && !host.settings->ssr &&
              host.settings->sss && host.settings->quality == api::Quality::reference,
          "a set changes the members given, nested ones too; the rest keep their value");
    CHECK(set.json("/result/rtGI/rays") == "8" && set.json("/result/ssr") == "false", "and answers the settings now in effect");
    CHECK(s.changeCount() == changes + 1, "a set counts as a change (a lockstep window redraws)");

    changes = s.changeCount();
    CHECK(Reply(s.call("render.settings", R"({"set":{"rtGI":{"rays":33}}})")).code() == "E_RANGE" &&
              Reply(s.call("render.settings", R"({"set":{"rtShadows":{"maxLights":5}}})")).code() == "E_RANGE" &&
              Reply(s.call("render.settings", R"({"set":{"csm":{"size":256}}})")).code() == "E_RANGE" &&
              Reply(s.call("render.settings", R"({"set":{"geometry":"triangles"}})")).code() == "E_ENUM" &&
              Reply(s.call("render.settings", R"({"set":{"ssgi":true}})")).code() == "E_UNKNOWN_FIELD" &&
              Reply(s.call("render.settings", R"({"set":{"exposure":0}})")).code() == "E_RANGE",
          "limits, enum names and unknown settings are refused");
    CHECK(host.settingsSet == 1 && s.changeCount() == changes, "(before the host is touched)");

    // Captures default to the settings: quality reference (set above), ssao, exposure.
    (void)s.call("render.settings", R"({"set":{"ssao":true,"exposure":2.5}})");
    const Reply cap(s.call("render.capture", R"({"inline":true,"timings":true})"));
    CHECK(cap.ok() && !seen.empty() && seen.back().quality == "reference" &&
              seen.back().post == std::vector<std::string>{"ssao"} && seen.back().exposure == 2.5f && seen.back().timings,
          "a capture that names none takes quality, post (ssao) and exposure from the settings");
    CHECK(cap.json("/result/gpuMs") == "1.235" &&
              cap.json("/result/timings") == R"([["gbuffer.early",0.071],["lighting",0.27]])",
          "gpuMs and the pass list come back, in microseconds");
    (void)s.call("render.capture", R"({"inline":true,"quality":"realtime","post":[],"exposure":1})");
    CHECK(seen.back().quality == "realtime" && seen.back().post.empty() && seen.back().exposure == 1.0f &&
              !seen.back().timings,
          "a capture's own quality, post and exposure override the settings");
    CHECK(!Reply(s.call("render.capture", R"({"inline":true})")).at("/result/timings"), "no timings unless asked");
}

// render.stats (2b test 14, stats part): params checked by the session (frames 1-120), the
// host's medians as [name, ms] pairs in microseconds, E_STATE before a frame was rendered
// (the CLI host before a capture), E_COMMAND without a host, and never a world change.
static void sp2b_t14_render_stats_reports_the_hosts_medians() {
    World w;
    api::Session s(w, engineTypes());
    CHECK(Reply(s.call("render.stats")).code() == "E_COMMAND", "without a renderer render.stats is E_COMMAND");
    FakeHost host;
    host.hostKind = api::HostKind::Cli;
    host.fn = [](const api::RenderRequest&) { return api::RenderOutcome{}; };
    s.setHost(&host);
    const Reply none(s.call("render.stats"));
    CHECK(none.code() == "E_STATE" && host.statsAsked == std::vector<int>{1},
          "a host that has rendered nothing: E_STATE (frames defaults to 1)");

    api::RenderStats st;
    st.frame = 41;
    st.frames = 10;
    st.gpuMs = 5.12345;
    st.cpuMs = 0.4;
    st.passes = {{"gbuffer.late", 0.0712843}, {"rt.gi", 2.5}, {"post.tonemap", 0.0004}};
    host.stats = st;
    const uint64_t changes = s.changeCount();
    const Reply r(s.call("render.stats", R"({"frames":10})"));
    CHECK(r.ok() && host.statsAsked.back() == 10, "frames reaches the host");
    CHECK(r.json("/result/frame") == "41" && r.json("/result/frames") == "10" && r.json("/result/gpuMs") == "5.123" &&
              r.json("/result/cpuMs") == "0.4",
          "the frame, the frames covered and the medians, in microseconds");
    CHECK(r.json("/result/passes") == R"([["gbuffer.late",0.071],["rt.gi",2.5],["post.tonemap",0]])",
          "passes as [name, ms] pairs, in the host's (frame) order");
    CHECK(!Reply(s.call("render.stats", R"({"passes":false})")).at("/result/passes"), "passes: false leaves them out");
    CHECK(s.changeCount() == changes, "reading stats does not count as a change");

    const std::size_t asked = host.statsAsked.size();
    CHECK(Reply(s.call("render.stats", R"({"frames":0})")).code() == "E_RANGE" &&
              Reply(s.call("render.stats", R"({"frames":121})")).code() == "E_RANGE" &&
              Reply(s.call("render.stats", R"({"frames":"all"})")).code() == "E_TYPE" &&
              Reply(s.call("render.stats", R"({"frame":5})")).code() == "E_UNKNOWN_FIELD",
          "frames outside 1-120, not a number, or an unknown param is refused");
    CHECK(host.statsAsked.size() == asked, "(before the host is asked)");
}

// render.capture (step 9): checked by the session, carried out by a host. No host: E_COMMAND.
static void t09_render_capture_needs_a_host_and_checks_its_params() {
    World w;
    api::Session s(w, engineTypes());
    CHECK(Reply(s.call("render.capture", R"({"path":"a.png"})")).code() == "E_COMMAND",
          "without a renderer render.capture is E_COMMAND");
    std::vector<api::RenderRequest> seen;
    FakeHost host;
    host.fn = [&](const api::RenderRequest& r) {
        seen.push_back(r);
        api::RenderOutcome o;
        o.frames = r.frames ? r.frames : 7;
        if (r.path == "fail.png") {
            reflect::Diagnostic d;
            d.code = "E_RENDER";
            d.message = "cannot write fail.png";
            o.errors.push_back(d);
        }
        return o;
    };
    s.setHost(&host);
    const Reply ok(s.call("render.capture", R"({"path":"a.png","mode":"rt_offline","spp":4,"size":1})"));
    CHECK(ok.code() == "E_UNKNOWN_FIELD" && ok.str("/error/data/0/path") == "/size", "unknown params are refused");
    const Reply good(s.call("render.capture", R"({"path":"a.png","mode":"rt_offline","spp":4,"width":640,)"
                                              R"("height":360,"denoise":"none","post":["taa","ssao"],"exposure":1.5})"));
    CHECK(good.ok() && good.str("/result/path") == "a.png" && good.str("/result/frames") == "7" && seen.size() == 1,
          "a valid capture reaches the host and reports frames and ms");
    const api::RenderRequest& r = seen.back();
    const std::vector<std::string> effects{"taa", "ssao"};
    CHECK(r.mode == "rt_offline" && r.spp == 4 && r.width == 640u && r.height == 360u && r.denoise == "none" &&
              r.post == effects && r.exposure == 1.5f && r.frames == 0 && !r.inlineImage && !r.region,
          "with every param as given");
    CHECK(Reply(s.call("render.capture", R"({"path":"a.png","mode":"raster"})")).code() == "E_ENUM" &&
              Reply(s.call("render.capture", R"({"path":"a.png","post":["glow"]})")).code() == "E_ENUM" &&
              Reply(s.call("render.capture", R"({"path":"a.png","width":8})")).code() == "E_RANGE" &&
              Reply(s.call("render.capture", R"({"path":"a.png","exposure":0})")).code() == "E_RANGE" &&
              Reply(s.call("render.capture", "{}")).code() == "E_COMMAND",
          "a bad mode, effect, size or exposure, or no path, is refused before the host is called");
    CHECK(seen.size() == 1, "(the host saw none of them)");
    CHECK(Reply(s.call("render.capture", R"({"path":"fail.png"})")).code() == "E_RENDER",
          "the host's errors are the reply's");
    CHECK(seen.size() == 2 && !seen.back().exposure && seen.back().mode == "deferred" && seen.back().post.empty() &&
              !seen.back().width && !seen.back().height,
          "without exposure the host keeps the renderer's own; the mode defaults to deferred, effects to none, the "
          "size to the host's");
}

// render.capture's preview (step 9): inline without a path, the image beside the result
// (Session::call(string) puts it in the result; the parsed call hands it over separately for
// MCP's image block), region and format checked by the session.
static void sp4_t16_an_inline_capture_carries_its_image_beside_the_result() {
    World w;
    api::Session s(w, engineTypes());
    std::vector<api::RenderRequest> seen;
    FakeHost host;
    host.fn = [&](const api::RenderRequest& r) {
        seen.push_back(r);
        api::RenderOutcome o;
        o.frames = 1;
        o.width = 1280;
        o.height = 720;
        if (r.inlineImage) {
            o.image = std::string("\xff\xd8\xff", 3);   // base64: "/9j/"
            o.imageMime = "image/jpeg";
            o.imageWidth = 768;
            o.imageHeight = 432;
        }
        return o;
    };
    s.setHost(&host);
    CHECK(Reply(s.call("world.info")).str("/result/host") == "headless", "setHost sets the host kind");

    const Reply r(s.call("render.capture", R"({"inline":true})"));
    CHECK(r.ok() && !r.at("/result/path") && r.str("/result/image") == "/9j/" &&
              r.str("/result/mimeType") == "image/jpeg" && r.str("/result/imageSize/0") == "768" &&
              r.str("/result/width") == "1280" && r.json("/result/deterministic") == "true",
          "inline without a path: the preview in the result, base64, with its size and the frame's");
    CHECK(seen.back().inlineImage && seen.back().path.empty() && seen.back().format == "jpeg" &&
              seen.back().maxSize == 768,
          "the host is asked for a JPEG preview at most 768 px");

    const auto doc = ecs::parseJson(R"({"inline":true,"format":"png","maxSize":256,"region":[10,20,300,200]})");
    const api::CallOutcome o = s.call("render.capture", doc.value(), doc.value().root());
    CHECK(o.status == api::CallStatus::Ok && o.image && o.image->base64 == "/9j/" &&
              o.result.find("\"image\"") == std::string::npos,
          "the parsed call hands the image over beside a result without it (MCP's image block)");
    const auto& req = seen.back();
    const std::array<int32_t, 4> wantRegion{10, 20, 300, 200};
    CHECK(req.format == "png" && req.maxSize == 256 && req.region && *req.region == wantRegion,
          "format, maxSize and region reach the host");

    const std::size_t before = seen.size();
    CHECK(Reply(s.call("render.capture", R"({"inline":true,"region":[0,0,300]})")).code() == "E_COMMAND" &&
              Reply(s.call("render.capture", R"({"inline":true,"region":[0,0,0,10]})")).code() == "E_COMMAND" &&
              Reply(s.call("render.capture", R"({"inline":true,"format":"gif"})")).code() == "E_ENUM" &&
              Reply(s.call("render.capture", R"({"inline":true,"maxSize":8})")).code() == "E_RANGE" &&
              Reply(s.call("render.capture", R"({"path":""})")).code() == "E_COMMAND" &&
              Reply(s.call("render.capture", R"({"inline":false})")).code() == "E_COMMAND",
          "a region that is not [x, y, w, h], a bad format or size, an empty path, or neither path nor inline");
    CHECK(seen.size() == before, "(none reached the host)");
}

// Session::changeCount (step 9): a window host renders in lockstep only when the world may
// have moved, so the count rises for the methods that change it and for steps, not reads.
static void sp4_t16_change_count_rises_only_for_methods_that_change_the_world() {
    World w;
    api::Session s(w, engineTypes());
    uint64_t last = s.changeCount();
    const auto moved = [&] {
        const bool m = s.changeCount() != last;
        last = s.changeCount();
        return m;
    };
    (void)s.call("world.info");
    (void)s.call("world.query", R"({"limit":1})");
    (void)s.call("world.hash");
    (void)s.call("world.snapshot", R"({"name":"a"})");
    (void)s.call("time.get");
    (void)s.call("input.state");
    (void)s.call("combat.info");
    (void)s.call("combat.events");
    CHECK(!moved(), "reads, snapshots and time.get do not count");
    (void)s.call("world.apply", R"({"commands":[{"op":"entity.create","name":"A"}]})");
    CHECK(moved(), "world.apply counts");
    (void)s.call("world.step", R"({"frames":2})");
    CHECK(moved(), "world.step counts");
    (void)s.call("world.restore", R"({"name":"a"})");
    CHECK(moved(), "world.restore counts");
    s.step({}, 1.0f / 60.0f);
    CHECK(moved(), "a host's step counts");
    (void)s.call("world.apply", R"({"commands":[{"op":"entity.destroy","id":"ffffffffffffffff"}]})");
    CHECK(moved(), "a failed batch counts too (cheap: one render it did not need)");
}

// ---- t09: commands ------------------------------------------------------------------------

static void t09_each_command_applies_and_returns_its_inverse() {
    struct Case {
        const char* command;
        bool (*effect)(const World&);
    };
    static const Case kCases[] = {
        {R"({"op":"entity.create","name":"Box","parent":"1000000000000001","components":{"Light":{"intensity":2}}})",
         [](const World& w) { return w.idCount() == 5; }},
        {R"({"op":"entity.destroy","id":"1000000000000002"})",
         [](const World& w) { return w.idCount() == 2 && byHex(w, kShade) == entt::null; }},
        {R"({"op":"entity.destroy","id":"1000000000000002","recursive":false})",
         [](const World& w) {
             return w.idCount() == 3 && !w.registry().all_of<Parent>(byHex(w, kShade));
         }},
        {R"({"op":"entity.reparent","id":"1000000000000004","parent":null})",
         [](const World& w) { return !w.registry().all_of<Parent>(byHex(w, kShade)); }},
        {R"({"op":"entity.reparent","id":"1000000000000003","parent":"1000000000000004"})",
         [](const World& w) { return w.registry().get<Parent>(byHex(w, kSun)).entity == byHex(w, kShade); }},
        {R"({"op":"component.set","id":"1000000000000001","type":"Light","value":{"type":"Point"}})",
         [](const World& w) { return w.registry().all_of<Light>(byHex(w, kRoom)); }},
        {R"({"op":"component.set","id":"1000000000000002","type":"Light","value":{"intensity":1}})",
         [](const World& w) { return w.registry().get<Light>(byHex(w, kLamp)).type == LightType::Point; }},
        {R"({"op":"component.set","id":"1000000000000004","type":"Parent","value":{"entity":"1000000000000003"}})",
         [](const World& w) { return w.registry().get<Parent>(byHex(w, kShade)).entity == byHex(w, kSun); }},
        {R"({"op":"component.remove","id":"1000000000000003","type":"Light"})",
         [](const World& w) { return !w.registry().all_of<Light>(byHex(w, kSun)); }},
        {R"({"op":"component.remove","id":"1000000000000004","type":"Parent"})",
         [](const World& w) { return !w.registry().all_of<Parent>(byHex(w, kShade)); }},
        {R"({"op":"field.set","id":"1000000000000002","path":"/Light/intensity","value":99})",
         [](const World& w) { return w.registry().get<Light>(byHex(w, kLamp)).intensity == 99.0f; }},
        {R"({"op":"field.set","id":"1000000000000004","path":"/Transform/position/1","value":7})",
         [](const World& w) { return w.registry().get<Transform>(byHex(w, kShade)).position.y == 7.0f; }},
        {R"({"op":"field.set","id":"1000000000000002","path":"/Light/color/2","value":0.25})",
         [](const World& w) { return w.registry().get<Light>(byHex(w, kLamp)).color.z == 0.25f; }},
        {R"({"op":"field.set","id":"1000000000000004","path":"/Parent/entity","value":"1000000000000001"})",
         [](const World& w) { return w.registry().get<Parent>(byHex(w, kShade)).entity == byHex(w, kRoom); }},
        {R"({"op":"field.set","id":"1000000000000003","path":"/Name/value","value":"Star"})",
         [](const World& w) { return w.registry().get<Name>(byHex(w, kSun)).value == "Star"; }},
        {R"({"op":"entity.duplicate","id":"1000000000000002"})",
         [](const World& w) { return w.idCount() == 6; }},
        {R"({"op":"entity.duplicate","id":"1000000000000002","recursive":false,"parent":null,"name":"Lamp 2"})",
         [](const World& w) { return w.idCount() == 5; }},
    };
    for (const Case& c : kCases) {
        World w;
        loadFile(w, "rooms.scene.json");
        api::Session s(w, engineTypes());
        const std::string before = save(w);
        const Reply r = applyBatch(s, std::string("[") + c.command + "]");
        if (!r.ok()) std::printf("\n      %s -> %s", c.command, r.text.c_str());
        CHECK(r.ok() && r.str("/result/applied") == "1", "the command applies");
        CHECK(c.effect(w), "and has its effect on the world");
        CHECK(save(w) != before, "which shows in the save");
        const Reply u = undo(s, r);
        CHECK(u.ok() && save(w) == before, "its inverse restores the save byte-identically");
    }
}

// entity.duplicate (step 10, the add-an-API-command recipe followed once).
static void t09_duplicate_copies_a_subtree_with_fresh_ids() {
    World w;
    loadFile(w, "rooms.scene.json");
    w.seedIds(5);
    api::Session s(w, engineTypes());
    const std::string before = save(w);
    // The whole Room tree under a new name, then an edit of the copy through "$0".
    const Reply r = applyBatch(s, R"([{"op":"entity.duplicate","id":"1000000000000001","name":"Room B"},
                                      {"op":"field.set","id":"$0","path":"/Transform/position/0","value":20}])");
    if (!r.ok()) std::printf("\n      %s", r.text.c_str());
    CHECK(r.ok() && w.idCount() == 7, "Room, Lamp and Shade are copied");
    const entt::entity room2 = w.findById(std::stoull(r.str("/result/results/0"), nullptr, 16));
    CHECK(room2 != entt::null && w.registry().get<Name>(room2).value == "Room B" &&
              !w.registry().all_of<Parent>(room2) && w.registry().get<Transform>(room2).position.x == 20.0f,
          "the result is the copy's id; it is named, a root like its source, and editable as $0");
    const auto& kids = w.registry().get<Children>(room2).entities;
    CHECK(kids.size() == 1 && w.registry().get<Name>(kids[0]).value == "Lamp" &&
              w.registry().get<Light>(kids[0]).intensity == 12.5f &&
              w.registry().get<Light>(kids[0]).type == LightType::Spot,
          "the copied Lamp hangs under the copy, with its Light");
    const auto& grand = w.registry().get<Children>(kids[0]).entities;
    CHECK(grand.size() == 1 && w.registry().get<Name>(grand[0]).value == "Shade" &&
              w.registry().get<Transform>(grand[0]).scale == glm::vec3(0.5f),
          "and the copied Shade under the copied Lamp");
    CHECK(w.registry().get<Children>(byHex(w, kLamp)).entities.size() == 1, "the source tree is untouched");
    const Reply u = undo(s, r);
    CHECK(u.ok() && save(w) == before, "the inverse removes the copy: byte-identical save");

    const Reply cycle = applyBatch(s, R"([{"op":"entity.duplicate","id":"1000000000000002","parent":"1000000000000004"}])");
    CHECK(cycle.code() == "E_PARENT_CYCLE" && cycle.str("/error/data/0/path") == "/commands/0/parent" &&
              save(w) == before,
          "a copy cannot go under an entity it copies");
    CHECK(Reply(s.call("world.apply", R"({"commands":[{"op":"entity.create","name":"Cam","parent":"1000000000000004",)"
                                      R"("components":{"Camera":{}}}]})")).ok(),
          "(a camera under the Shade)");
    const Reply single = applyBatch(s, R"([{"op":"entity.duplicate","id":"1000000000000002"}])");
    CHECK(single.code() == "E_SINGLETON" && w.idCount() == 5, "a subtree holding a singleton cannot be copied");
    CHECK(applyBatch(s, R"([{"op":"entity.duplicate","id":"1000000000000002","recursive":false}])").ok() &&
              w.idCount() == 6,
          "the entity alone can");
    CHECK(applyBatch(s, R"([{"op":"entity.duplicate","id":"1000000000000002","deep":true}])").code() ==
              "E_UNKNOWN_FIELD",
          "unknown members are refused");
}

static void t09_failing_command_rolls_back_the_batch() {
    World w;
    loadFile(w, "rooms.scene.json");
    w.seedIds(77);
    api::Session s(w, engineTypes());
    const std::string before = save(w);
    const World::IdGenState gen = w.idGenState();
    const uint64_t seq = w.nextCreationSeq();
    const Reply r = applyBatch(s, R"([
        {"op":"entity.create","name":"New","components":{"Light":{"intensity":4}}},
        {"op":"field.set","id":"$0","path":"/Transform/position/0","value":4},
        {"op":"entity.destroy","id":"1000000000000002"},
        {"op":"entity.reparent","id":"1000000000000003","parent":"$0"},
        {"op":"field.set","id":"1000000000000003","path":"/Light/intensity","value":-1}])");
    CHECK(r.code() == "E_RANGE" && r.str("/error/data/0/path") == "/commands/4/value",
          "the fifth command fails with E_RANGE at its path");
    CHECK(r.str("/error/data/0/entity") == kSun, "naming the entity");
    CHECK(r.at("/error/data")->count == 1, "and nothing else (the rollback itself succeeded)");
    CHECK(save(w) == before, "the four applied commands are rolled back: byte-identical save");
    CHECK(w.idGenState() == gen && w.nextCreationSeq() == seq, "the id generator and creation sequence too");
    World fresh;
    loadFile(fresh, "rooms.scene.json");
    fresh.seedIds(77);
    CHECK(w.idOf(w.create("x")) == fresh.idOf(fresh.create("x")), "so the next generated id is unchanged");

    const Reply first = applyBatch(s, R"([{"op":"entity.destroy","id":"00000000000000ff"}])");
    CHECK(first.code() == "E_ENTITY_NOT_FOUND" && first.str("/error/data/0/path") == "/commands/0/id",
          "a failing first command changes nothing either");
}

// Camera and Environment are one per world on the command path too (step 8).
static void t20_commands_keep_one_camera_and_environment() {
    World w;
    loadFile(w, "rooms.scene.json");
    api::Session s(w, engineTypes());
    const std::string before = save(w);
    const Reply two = applyBatch(s, R"([
        {"op":"entity.create","name":"CamA","components":{"Camera":{"fovDeg":50}}},
        {"op":"entity.create","name":"CamB","components":{"Camera":{"active":false}}}])");
    CHECK(two.code() == "E_SINGLETON" && two.str("/error/data/0/path").rfind("/commands/1", 0) == 0,
          "a second Camera in one batch fails at the second command");
    CHECK(save(w) == before, "and the batch is rolled back");
    const Reply one = applyBatch(s, R"([{"op":"component.set","id":"1000000000000001","type":"Environment",)"
                                    R"("value":{"hdri":{"path":"assets/test_models/env_studio.hdr"}}}])");
    CHECK(one.ok(), "one Environment is accepted");
    const Reply again = applyBatch(s, R"([{"op":"component.set","id":"1000000000000001","type":"Environment",)"
                                      R"("value":{"intensity":2}}])");
    CHECK(again.ok(), "replacing the same entity's Environment is not a second one");
    const Reply other = applyBatch(s, R"([{"op":"component.set","id":"1000000000000003","type":"Environment",)"
                                      R"("value":{}}])");
    CHECK(other.code() == "E_SINGLETON", "another entity's Environment is E_SINGLETON");
    CHECK(undo(s, again).ok() && undo(s, one).ok() && save(w) == before, "the inverses restore the file byte-identically");
}

// A deterministic world through the public World API: hierarchy and lights.
static void buildWorld(World& w, uint64_t seed, int n) {
    w.seedIds(seed);
    std::mt19937_64 rng(seed);
    std::vector<entt::entity> es;
    for (int i = 0; i < n; ++i) {
        Transform t;
        t.position = {static_cast<float>(rng() % 200) / 8.0f - 12.0f, static_cast<float>(rng() % 50) / 4.0f,
                      static_cast<float>(rng() % 200) / 8.0f - 12.0f};
        const auto e = w.create("E" + std::to_string(i), t);
        if (i > 0 && rng() % 3 != 0) w.setParent(e, es[rng() % es.size()]);
        if (rng() % 4 == 0) {
            Light l;
            l.intensity = static_cast<float>(rng() % 50);
            w.registry().emplace<Light>(e, l);
        }
        es.push_back(e);
    }
}

static std::vector<uint64_t> liveIds(const World& w) {
    std::vector<uint64_t> ids;
    for (auto [e, id] : w.registry().view<const EntityId>().each()) ids.push_back(id.value);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Random commands are tried one at a time on a mirror world; the ones that apply form
// one batch for the world under test. So the batch is valid in sequence, mixes every op,
// and its effect is known (the mirror's save).
static void t09_batch_then_inverse_is_byte_identical() {
    World a, b;
    for (World* w : {&a, &b}) buildWorld(*w, 9, 120);
    api::Session sa(a, engineTypes()), sb(b, engineTypes());
    for (api::Session* s : {&sa, &sb}) {
        const Reply r = applyBatch(*s, R"([{"op":"entity.create","id":"00000000000000c1","name":"Speaker",
            "components":{"Transform":{"position":[3,1,2]}},
            "preserved":{"AudioSource":{"version":2,"value":{"clip":{"path":"hum.ogg"},"volume":0.4}}}}])");
        CHECK(r.ok(), "an entity with a preserved unknown component is created");
    }
    CHECK(save(a).find(R"("AudioSource": 2)") != std::string::npos, "and its component saves with its version");

    // A root with children, for the non-recursive destroy.
    uint64_t parentId = 0;
    for (auto [e, ch] : b.registry().view<const Children>().each())
        if (!ch.entities.empty() && !b.registry().all_of<Parent>(e) && (parentId == 0 || b.idOf(e) < parentId))
            parentId = b.idOf(e);
    CHECK(parentId != 0, "the world has a root with children");

    std::vector<std::string> kept;
    std::map<std::string, int> opCount;
    const auto tryCommand = [&](const std::string& cmd, const std::string& op) {
        if (applyBatch(sb, "[" + cmd + "]").ok()) {
            kept.push_back(cmd);
            ++opCount[op];
        }
    };
    tryCommand(R"({"op":"entity.destroy","id":"00000000000000c1"})", "entity.destroy");
    {
        JsonWriter c;
        c.beginObject();
        c.key("op"); c.string("entity.destroy");
        c.key("id"); c.hex64(parentId);
        c.key("recursive"); c.boolean(false);
        c.endObject();
        tryCommand(c.str(), "entity.destroy");
    }
    CHECK(kept.size() == 2, "the scripted destroys apply on the mirror");

    std::mt19937_64 rng(4242);
    const char* kTypes[] = {"Point", "Directional", "Spot", "AreaRect"};
    for (int i = 0; i < 400; ++i) {
        const std::vector<uint64_t> ids = liveIds(b);
        const auto pick = [&] { return ids[rng() % ids.size()]; };
        const int kind = ids.empty() ? 0 : static_cast<int>(rng() % 10);
        JsonWriter c;
        c.beginObject();
        std::string op;
        const auto head = [&](const char* name, bool withId) {
            op = name;
            c.key("op");
            c.string(name);
            if (withId) {
                c.key("id");
                c.hex64(pick());
            }
        };
        switch (kind) {
        case 0:
            head("entity.create", false);
            c.key("name"); c.string("N" + std::to_string(i));
            if (!ids.empty() && rng() % 2) { c.key("parent"); c.hex64(pick()); }
            if (rng() % 2) {
                c.key("components"); c.beginObject();
                c.key("Light"); c.beginObject(); c.key("intensity"); c.int64(static_cast<int64_t>(rng() % 20)); c.endObject();
                c.key("Transform"); c.beginObject(); c.key("position"); c.beginArray();
                c.int64(1); c.int64(static_cast<int64_t>(rng() % 9)); c.int64(-2); c.endArray(); c.endObject();
                c.endObject();
            }
            break;
        case 1:
            head("entity.destroy", true);
            c.key("recursive"); c.boolean(rng() % 2 == 0);
            break;
        case 2:
            head("entity.reparent", true);
            c.key("parent");
            if (rng() % 4 == 0) c.null();
            else c.hex64(pick());
            break;
        case 3:
            head("component.set", true);
            c.key("type"); c.string("Light");
            c.key("value"); c.beginObject();
            c.key("type"); c.string(kTypes[rng() % 4]);
            c.key("intensity"); c.int64(static_cast<int64_t>(rng() % 30));
            c.endObject();
            break;
        case 4:
            head("component.remove", true);
            c.key("type"); c.string("Light");
            break;
        case 5:
            head("field.set", true);
            c.key("path"); c.string("/Light/intensity");
            c.key("value"); c.int64(static_cast<int64_t>(rng() % 12) - 2);   // negatives fail
            break;
        case 6:
            head("field.set", true);
            c.key("path"); c.string("/Transform/position/" + std::to_string(rng() % 3));
            c.key("value"); c.float32(static_cast<float>(rng() % 1000) / 7.0f);
            break;
        case 7:
            head("field.set", true);
            c.key("path"); c.string("/Name/value");
            c.key("value"); c.string("R" + std::to_string(i));
            break;
        case 8:
            head("component.set", true);
            c.key("type"); c.string("Transform");
            c.key("value"); c.beginObject(); c.key("scale"); c.beginArray(); c.int64(2); c.int64(2); c.int64(2);
            c.endArray(); c.endObject();
            break;
        default:
            head("field.set", true);
            c.key("path"); c.string("/Parent/entity");
            c.key("value");
            if (rng() % 3 == 0) c.null();
            else c.hex64(pick());
            break;
        }
        c.endObject();
        tryCommand(c.str(), op);
    }
    CHECK(kept.size() > 200, "most random commands apply");
    for (const char* op : {"entity.create", "entity.destroy", "entity.reparent", "component.set", "component.remove",
                           "field.set"})
        CHECK(opCount[op] >= 5, "every op is in the batch");

    const std::string before = save(a);
    std::string batch = "[";
    for (std::size_t i = 0; i < kept.size(); ++i) batch += (i ? "," : "") + kept[i];
    batch += "]";
    const Reply r = applyBatch(sa, batch);
    if (!r.ok()) std::printf("\n      %s", r.text.substr(0, 400).c_str());
    CHECK(r.ok() && r.str("/result/applied") == std::to_string(kept.size()), "the whole batch applies");
    const std::string after = save(a);
    CHECK(after == save(b), "with exactly the effect the commands had one by one");
    const Reply u = undo(sa, r);
    CHECK(u.ok(), "its inverse applies");
    CHECK(save(a) == before, "and restores the world byte-identically");
    const Reply redo = undo(sa, u);
    CHECK(redo.ok() && save(a) == after, "the inverse of the inverse redoes the batch");
}

static void t09_command_built_world_saves_like_the_file() {
    for (const char* file : {"rooms.scene.json", "agent_minimal.scene.json", "preserved.scene.json"}) {
        World fromFile;
        loadFile(fromFile, file);
        auto parsed = parseJson(readFile(kScenes / "v1" / file));
        CHECK(parsed, "the corpus file parses");
        const JsonDocument& doc = parsed.value();
        const JsonNode& ents = *doc.member(doc.root(), "entities");
        const JsonNode* header = doc.member(doc.root(), "components");
        // Every entity becomes one entity.create; unknown components go to "preserved".
        JsonWriter c;
        c.beginArray();
        for (uint32_t i = 0; i < ents.count; ++i) {
            const JsonNode& en = doc.child(ents, i);
            c.beginObject();
            c.key("op"); c.string("entity.create");
            c.key("id"); c.string(doc.member(en, "id")->text);
            c.key("components"); c.beginObject();
            for (uint32_t m = 0; m < en.count; ++m) {
                const JsonNode& comp = doc.child(en, m);
                if (comp.key == "id" || !engineTypes().find(comp.key)) continue;
                c.key(comp.key);
                reflect::writeJson(c, doc, comp);
            }
            c.endObject();
            bool any = false;
            for (uint32_t m = 0; m < en.count; ++m) {
                const JsonNode& comp = doc.child(en, m);
                if (comp.key == "id" || engineTypes().find(comp.key)) continue;
                if (!any) { c.key("preserved"); c.beginObject(); any = true; }
                c.key(comp.key); c.beginObject();
                c.key("version"); c.numberToken(doc.member(*header, comp.key)->text);
                c.key("value"); reflect::writeJson(c, doc, comp);
                c.endObject();
            }
            if (any) c.endObject();
            c.endObject();
        }
        c.endArray();
        World built;
        api::Session s(built, engineTypes());
        const Reply r = applyBatch(s, c.str());
        if (!r.ok()) std::printf("\n      %s: %s", file, r.text.c_str());
        CHECK(r.ok(), "the file's entities apply as entity.create commands");
        CHECK(save(built) == save(fromFile), "the command-built world saves byte-identically to the loaded file");
    }
}

static void t09_dollar_refs_name_entities_created_in_the_batch() {
    World w;
    api::Session s(w, engineTypes());
    const Reply r = applyBatch(s, R"([{"op":"entity.create","name":"Group"},
        {"op":"entity.create","name":"Lamp","parent":"$0","components":{"Light":{"intensity":3}}},
        {"op":"field.set","id":"$1","path":"/Transform/position/1","value":2.5}])");
    CHECK(r.ok(), "a batch can build a group and edit what it just created");
    const std::string group = r.str("/result/results/0"), lamp = r.str("/result/results/1");
    CHECK(group.size() == 16 && lamp.size() == 16 && r.json("/result/results/2") == "null",
          "results give each created id, null for other commands");
    const auto& reg = w.registry();
    const auto e = byHex(w, lamp.c_str());
    CHECK(reg.get<Parent>(e).entity == byHex(w, group.c_str()) && reg.get<Transform>(e).position.y == 2.5f,
          "\"$k\" resolved to command k's entity");
    CHECK(applyBatch(s, R"([{"op":"field.set","id":"$0","path":"/Name/value","value":"x"}])").code() == "E_ID_INVALID",
          "\"$k\" must name an earlier entity.create");
}

static void t09_command_errors_are_precise_and_terse() {
    World w;
    loadFile(w, "rooms.scene.json");
    api::Session s(w, engineTypes());
    const std::string before = save(w);
    const auto one = [&](const std::string& cmd) { return applyBatch(s, "[" + cmd + "]"); };

    const Reply range = one(R"({"op":"field.set","id":"1000000000000002","path":"/Light/intensity","value":-1})");
    CHECK(range.code() == "E_RANGE" && range.str("/error/data/0/path") == "/commands/0/value" &&
              range.str("/error/data/0/expected") == ">= 0",
          "field.set below min is E_RANGE at /commands/0/value with the limit");
    CHECK(range.text.size() < 400, "one error stays under ~100 tokens");
    CHECK(one(R"({"op":"field.set","id":"1000000000000002","path":"/Light/innerConeDeg","value":200})").code() ==
              "E_RANGE",
          "above max too");
    const Reply typo = one(R"({"op":"field.set","id":"1000000000000002","path":"/Light/intensty","value":1})");
    CHECK(typo.code() == "E_PATH" && typo.str("/error/data/0/suggestion") == "intensity" &&
              typo.str("/error/data/0/path") == "/commands/0/path",
          "a misspelled field is E_PATH with a suggestion");
    const Reply comp = one(R"({"op":"field.set","id":"1000000000000002","path":"/Ligth/intensity","value":1})");
    CHECK(comp.code() == "E_COMPONENT_UNKNOWN" && comp.str("/error/data/0/suggestion") == "Light",
          "a misspelled component is E_COMPONENT_UNKNOWN with a suggestion");
    CHECK(one(R"({"op":"field.set","id":"1000000000000002","path":"/Children/entities","value":[]})").code() ==
              "E_COMPONENT_UNKNOWN",
          "runtime state cannot be set");
    CHECK(one(R"({"op":"field.set","id":"1000000000000002","path":"/Light","value":{}})").code() == "E_PATH",
          "a path must reach a field");
    CHECK(one(R"({"op":"field.set","id":"1000000000000002","path":"/Transform/position/3","value":1})").code() ==
              "E_PATH",
          "an index past the end is E_PATH");
    const Reply type = one(R"({"op":"field.set","id":"1000000000000002","path":"/Transform/position/1","value":"x"})");
    CHECK(type.code() == "E_TYPE" && type.str("/error/data/0/path") == "/commands/0/value",
          "a deep path reports a wrong value at /value");
    CHECK(one(R"({"op":"field.set","id":"1000000000000001","path":"/Light/intensity","value":1})").code() ==
              "E_COMMAND",
          "a field of a missing component is E_COMMAND");
    const Reply missing = one(R"({"op":"field.set","id":"00000000000000ff","path":"/Light/intensity","value":1})");
    CHECK(missing.code() == "E_ENTITY_NOT_FOUND" && missing.str("/error/data/0/path") == "/commands/0/id",
          "an unknown id is E_ENTITY_NOT_FOUND at /id");
    const Reply op = one(R"({"op":"field.sett","id":"1000000000000002"})");
    CHECK(op.code() == "E_COMMAND" && op.str("/error/data/0/suggestion") == "field.set", "a misspelled op");
    const Reply member = one(R"({"op":"entity.create","nmae":"x"})");
    CHECK(member.code() == "E_UNKNOWN_FIELD" && member.str("/error/data/0/suggestion") == "name",
          "a misspelled member");
    CHECK(one(R"({"op":"component.remove","id":"1000000000000002","type":"Name"})").code() == "E_COMMAND",
          "Name and Transform cannot be removed");
    CHECK(one(R"({"op":"entity.reparent","id":"1000000000000001","parent":"1000000000000004"})").code() ==
              "E_PARENT_CYCLE",
          "a cycle is E_PARENT_CYCLE");
    CHECK(one(R"({"op":"entity.create","id":"1000000000000001"})").code() == "E_ID_DUPLICATE",
          "a used id is E_ID_DUPLICATE");
    CHECK(one(R"({"op":"entity.create","parent":"00000000000000ff"})").code() == "E_PARENT_MISSING",
          "a missing parent is E_PARENT_MISSING");
    CHECK(one(R"({"op":"component.set","id":"1000000000000002","type":"Parent","value":{"entity":"00000000000000ff"}})")
                  .code() == "E_PARENT_MISSING",
          "also through a Parent value");
    CHECK(one(R"({"op":"entity.create","name":"x","components":{"Name":{"value":"y"}}})").code() == "E_DUPLICATE_KEY",
          "name and Name together are ambiguous");
    const Reply many = one(R"({"op":"entity.create","components":{"Light":{"intensity":-1,"range":-1,"colour":[1,1,1]}}})");
    CHECK(many.code() == "E_RANGE" && many.at("/error/data")->count == 3 &&
              many.str("/error/message").find("(+2 more)") != std::string::npos,
          "every error of the failing command is reported");
    CHECK(save(w) == before, "no failed command changed the world");
}

// ---- t09: queries and the summary (token budget) -------------------------------------------

static void t09_query_filters_and_projects() {
    World w;
    loadFile(w, "rooms.scene.json");
    api::Session s(w, engineTypes());
    const Reply lights(s.call("world.query", R"({"has":["Light"],"fields":["/Light/intensity"]})"));
    CHECK(lights.text == R"({"result":{"entities":[{"id":"1000000000000002","Light":{"intensity":12.5}},)"
                         R"({"id":"1000000000000003","Light":{"intensity":3}}],"count":2}})",
          "has + a field projection: exactly the asked-for values, in creation order");
    const Reply sun(s.call("world.query", R"({"names":["Sun"],"fields":["/Transform/position","/Name"]})"));
    CHECK(sun.text == R"({"result":{"entities":[{"id":"1000000000000003","Transform":{"position":[0,50,0]},)"
                      R"("Name":{"value":"Sun"}}],"count":1}})",
          "names + several projections");
    CHECK(Reply(s.call("world.query", R"({"nameContains":"a","fields":["/Name/value"]})")).str("/result/count") == "2",
          "nameContains is a substring match (Lamp, Shade)");
    const Reply byId(s.call("world.query", R"({"ids":["1000000000000004","00000000000000ff"],"fields":[]})"));
    CHECK(byId.str("/result/count") == "1" && byId.str("/result/missing/0") == "00000000000000ff",
          "ids that do not exist are listed as missing");
    CHECK(byId.json("/result/entities/0") == R"({"id":"1000000000000004"})", "fields: [] gives ids only");
    const Reply full(s.call("world.query", R"({"ids":["1000000000000002"]})"));
    const std::string file = save(w, reflect::JsonLayout::Compact);
    CHECK(file.find(full.json("/result/entities/0")) != std::string::npos,
          "without fields an entity is exactly its compact file form");
    const Reply page(s.call("world.query", R"({"limit":1,"offset":1,"fields":["/Name/value"]})"));
    CHECK(page.str("/result/entities/0/Name/value") == "Lamp" && page.str("/result/count") == "4" &&
              page.str("/result/next") == "2",
          "limit/offset page through the matches; next is the following offset");
    const Reply whole(s.call("world.query", R"({"ids":["1000000000000004"],"fields":["/Transform"]})"));
    const Reply wholeFull(
        s.call("world.query", R"({"ids":["1000000000000004"],"fields":["/Transform"],"defaults":true})"));
    CHECK(whole.json("/result/entities/0/Transform") == R"({"position":[0,-0.1,0],"scale":[0.5,0.5,0.5]})" &&
              wholeFull.json("/result/entities/0/Transform/rotation") == "[0,0,0,1]",
          "whole components omit defaults unless defaults:true");
    (void)s.call("world.step");
    const Reply wt(s.call("world.query", R"({"ids":["1000000000000002"],"fields":["/WorldTransform/world"]})"));
    CHECK(wt.at("/result/entities/0/WorldTransform/world") &&
              wt.at("/result/entities/0/WorldTransform/world")->count == 16,
          "runtime components can be read (world matrices)");
    CHECK(Reply(s.call("world.query", R"({"fields":["/Light/intensty"]})")).str("/error/data/0/suggestion") ==
              "intensity",
          "a bad projection is E_PATH with a suggestion");
}

static void t09_summary_is_small_and_complete() {
    World w;
    loadFile(w, "rooms.scene.json");
    api::Session s(w, engineTypes());
    const Reply r(s.call("world.summary"));
    CHECK(r.ok() && r.str("/result/entities") == "4" && r.str("/result/roots") == "2", "counts");
    CHECK(r.str("/result/components/Light") == "2" && r.str("/result/components/Parent") == "2" &&
              r.str("/result/components/Name") == "4",
          "entities per component");
    CHECK(r.json("/result/bounds/min") == "[0,0,-4]" && r.json("/result/bounds/max") == "[15,50,0]",
          "bounds of the entity origins in world space (children inherit the Room's scale)");
    CHECK(r.str("/result/outline/0/name") == "Room" && r.str("/result/outline/0/children/0/name") == "Lamp" &&
              r.str("/result/outline/0/children/0/children/0/name") == "Shade" &&
              r.str("/result/outline/1/name") == "Sun",
          "an outline of the hierarchy in creation order");
    const Reply shallow(s.call("world.summary", R"({"depth":0})"));
    CHECK(!shallow.at("/result/outline/0/children") && shallow.str("/result/outline/0/more") == "1",
          "below the depth limit only the number of children is given");

    World big;
    buildWorld(big, 3, 10'000);
    api::Session sb(big, engineTypes());
    const Reply bigSummary(sb.call("world.summary"));
    CHECK(bigSummary.ok() && bigSummary.str("/result/entities") == "10000", "a 10k world summarizes");
    CHECK(bigSummary.text.size() < 6000, "in under ~1.5k tokens (64 nodes)");
    CHECK(!bigSummary.str("/result/moreRoots").empty(), "and says how many roots it left out");
}

// ---- t09: scene methods --------------------------------------------------------------------

static void t09_scene_methods_load_save_validate() {
    World w;
    api::Session s(w, engineTypes());
    const std::string rooms = (kScenes / "v1" / "rooms.scene.json").generic_string();
    const Reply v(s.call("scene.validate", "{\"path\":\"" + rooms + "\"}"));
    CHECK(v.ok() && v.json("/result") == R"({"ok":true})", "a valid file validates with nothing else to say");
    const Reply bad(s.call("scene.validate",
                           R"({"scene":{"format":"ohao.scene","version":1,"entities":[{"id":"00000000000000a1","Light":{"intensty":1}}]}})"));
    CHECK(bad.ok() && bad.json("/result/ok") == "false" && bad.str("/result/errors/0/code") == "E_UNKNOWN_FIELD",
          "validation errors are a result, not a failure");
    const Reply loaded(s.call("scene.load", "{\"path\":\"" + rooms + "\",\"seed\":\"0000000000000005\"}"));
    CHECK(loaded.ok() && loaded.str("/result/entities") == "4", "scene.load loads the file");
    CHECK(Reply(s.call("scene.load", "{\"path\":\"" + rooms + "\"}")).code() == "E_WORLD_NOT_EMPTY",
          "into an empty world only");
    const Reply inline_(s.call("scene.save"));
    CHECK(inline_.json("/result/scene") == save(w, reflect::JsonLayout::Compact),
          "scene.save without a path returns the compact scene as an object");
    // Per process: CTest and the doc checker can run this suite at the same time.
    const fs::path out = fs::temp_directory_path() / ("ohao_api_test_" + std::to_string(processId()) + ".scene.json");
    const Reply file(s.call("scene.save", "{\"path\":\"" + out.generic_string() + "\"}"));
    CHECK(file.ok() && readFile(out) == save(w), "with a path it writes the pretty file");
    fs::remove(out);

    World w2;
    api::Session s2(w2, engineTypes());
    const Reply fromObject(s2.call("scene.load", "{\"scene\":" + inline_.json("/result/scene") + "}"));
    CHECK(fromObject.ok() && save(w2) == save(w), "scene.load also takes the scene as an object");
    const Reply step(s2.call("world.step", R"({"frames":3})"));
    CHECK(step.json("/result") == R"({"frame":3})" && s2.frame() == 3, "world.step counts frames");
    CHECK(Reply(s2.call("world.step", R"({"dt":0})")).code() == "E_RANGE", "dt must be positive");
    World w3;
    api::Session s3(w3, engineTypes());
    const Reply failed(s3.call("scene.load", R"({"text":"{\"format\":\"ohao.scene\",\"version\":1,\"entities\":[{\"id\":\"0\"}]}"})"));
    CHECK(failed.code() == "E_ID_INVALID" && w3.idCount() == 0, "a failed load leaves the world empty");
}

// ---- t10: snapshots through the API ---------------------------------------------------------

// The spec's script: snapshot, edit, step, query, restore -- and the world, the frame and
// the next generated id come back.
static void t10_snapshot_and_restore_through_the_api() {
    World w;
    loadFile(w, "rooms.scene.json");
    w.seedIds(99);
    api::Session s(w, engineTypes());
    const std::string before = save(w);
    const Reply snap(s.call("world.snapshot", R"({"name":"base"})"));
    CHECK(snap.ok() && snap.str("/result/name") == "base" && snap.str("/result/entities") == "4" &&
              std::stoul(snap.str("/result/bytes")) > 0,
          "world.snapshot reports the entities and the memory it holds");
    const Reply created = applyBatch(s, R"([{"op":"entity.create","name":"Extra"},
        {"op":"field.set","id":"1000000000000002","path":"/Light/intensity","value":2},
        {"op":"entity.destroy","id":"1000000000000003"},
        {"op":"entity.reparent","id":"1000000000000004","parent":null}])");
    CHECK(created.ok(), "edits apply");
    (void)s.call("world.step", R"({"frames":5})");
    CHECK(s.frame() == 5 && save(w) != before, "the world moved on");
    const Reply r(s.call("world.restore", R"({"name":"base"})"));
    CHECK(r.ok() && r.str("/result/created") == "1" && r.str("/result/destroyed") == "1" &&
              r.str("/result/changed") == "2" && r.str("/result/frame") == "0",
          "restore reports one recreated, one destroyed, two changed, and the frame");
    CHECK(save(w) == before && s.frame() == 0, "the save and the frame are back");
    const Reply again = applyBatch(s, R"([{"op":"entity.create","name":"Extra"}])");
    CHECK(again.str("/result/results/0") == created.str("/result/results/0"), "the next generated id repeats");
    const Reply unknown(s.call("world.restore", R"({"name":"bsae"})"));
    CHECK(unknown.code() == "E_SNAPSHOT_UNKNOWN" && unknown.str("/error/data/0/suggestion") == "base",
          "an unknown snapshot name is E_SNAPSHOT_UNKNOWN with a suggestion");
    CHECK(Reply(s.call("world.snapshot", "{}")).code() == "E_COMMAND", "a snapshot needs a name");
    const Reply resnap(s.call("world.snapshot", R"({"name":"base"})"));
    CHECK(resnap.ok() && resnap.str("/result/entities") == "5", "snapshotting a name again replaces it");
}

// ---- t11: assets through the API --------------------------------------------------------------

namespace {
// Knows one model path; never touches the disk.
struct OneFileResolver final : ecs::AssetResolver {
    Result<std::shared_ptr<const Model>, ecs::AssetError> loadModel(const ecs::AssetRef&, std::string_view = {}) override {
        return Result<std::shared_ptr<const Model>, ecs::AssetError>::err({"E_ASSET_NOT_FOUND", "fake"});
    }
    bool exists(const ecs::AssetRef& r) const override { return r.path == "props/chair.glb"; }
    std::filesystem::path absolutePath(const ecs::AssetRef& r) const override { return r.path; }
};
} // namespace

static void t11_validate_checks_assets_when_asked() {
    World w;
    api::Session s(w, engineTypes());
    const std::string scene =
        R"({"format":"ohao.scene","version":1,"entities":[)"
        R"({"id":"00000000000000a1","MeshSource":{"source":{"ModelAsset":{"asset":{"path":"props/chair.glb"}}}}},)"
        R"({"id":"00000000000000a2","MeshSource":{"source":{"ModelAsset":{"asset":{"path":"props/sofa.glb"}}}}}]})";
    CHECK(Reply(s.call("scene.validate", "{\"scene\":" + scene + ",\"checkAssets\":true}")).code() == "E_COMMAND",
          "checkAssets without a resolver is E_COMMAND");
    OneFileResolver files;
    s.setAssetResolver(&files);
    const Reply plain(s.call("scene.validate", "{\"scene\":" + scene + "}"));
    CHECK(plain.json("/result/ok") == "true", "without checkAssets, paths are not checked");
    const Reply checked(s.call("scene.validate", "{\"scene\":" + scene + ",\"checkAssets\":true}"));
    CHECK(checked.json("/result/ok") == "false" && checked.str("/result/errors/0/code") == "E_ASSET_NOT_FOUND" &&
              checked.str("/result/errors/0/path") == "/entities/1/MeshSource/source/ModelAsset/asset/path",
          "with it, the missing model is E_ASSET_NOT_FOUND at its path");
}

// ---- informational benchmark (step 6 gates the budgets) -------------------------------------

static void bench_apply_and_query_10k() {
    World w;
    buildWorld(w, 1, 10'000);
    api::Session s(w, engineTypes());
    const std::vector<uint64_t> ids = liveIds(w);

    JsonWriter c;
    c.beginObject();
    c.key("commands");
    c.beginArray();
    for (int i = 0; i < 100; ++i) {
        c.beginObject();
        c.key("op"); c.string("field.set");
        c.key("id"); c.hex64(ids[static_cast<std::size_t>(i) * 97]);
        c.key("path"); c.string("/Transform/position");
        c.key("value"); c.beginArray(); c.float32(i * 0.5f); c.float32(1.0f); c.float32(-2.25f); c.endArray();
        c.endObject();
    }
    c.endArray();
    c.endObject();
    const std::string batch = c.take();

    JsonWriter q;
    q.beginObject();
    q.key("ids");
    q.beginArray();
    for (int i = 0; i < 1000; ++i) q.hex64(ids[static_cast<std::size_t>(i) * 9]);
    q.endArray();
    q.key("limit"); q.int64(1000);
    q.endObject();
    const std::string query = q.take();

    const auto timeIt = [](auto&& fn) {
        for (int i = 0; i < 5; ++i) fn();
        std::vector<double> ms;
        for (int i = 0; i < 50; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        return std::pair{ms[25], ms[47]};
    };
    std::string applied, queried;
    const auto [applyMed, applyP95] = timeIt([&] { applied = s.call("world.apply", batch); });
    const auto [queryMed, queryP95] = timeIt([&] { queried = s.call("world.query", query); });
    std::string summary;
    const auto [sumMed, sumP95] = timeIt([&] { summary = s.call("world.summary"); });
    CHECK(Reply(applied).ok() && Reply(queried).str("/result/count") == "1000", "the benchmark calls succeed");

    // Reply sizes behind the spec's token estimates (bytes / 4 ~ tokens).
    JsonWriter oneQ;
    oneQ.beginObject();
    oneQ.key("ids"); oneQ.beginArray(); oneQ.hex64(ids[0]); oneQ.endArray();
    oneQ.key("fields"); oneQ.beginArray(); oneQ.string("/Transform/position"); oneQ.endArray();
    oneQ.endObject();
    const std::string oneReply = s.call("world.query", oneQ.str());
    JsonWriter fs1;
    fs1.beginObject();
    fs1.key("commands"); fs1.beginArray(); fs1.beginObject();
    fs1.key("op"); fs1.string("field.set");
    fs1.key("id"); fs1.hex64(ids[0]);
    fs1.key("path"); fs1.string("/Transform/position/1");
    fs1.key("value"); fs1.float32(2.5f);
    fs1.endObject(); fs1.endArray(); fs1.endObject();
    const std::string setReply = s.call("world.apply", fs1.str());
    const std::string errReply = s.call("world.apply", R"({"commands":[{"op":"field.set","id":")" +
                                                           std::string(Reply(oneReply).str("/result/entities/0/id")) +
                                                           R"(","path":"/Transform/positon","value":1}]})");
    const std::string scene = save(w, reflect::JsonLayout::Compact);
    std::printf("\n      10k: apply 100 field.set median %.3f ms (p95 %.3f), query 1000 ids full compact median "
                "%.3f ms (p95 %.3f, %.0f KB), summary median %.3f ms (p95 %.3f)"
                "\n      reply bytes: query 1 field %zu + request %zu, field.set %zu + request %zu, one error %zu, "
                "summary %zu, whole scene compact %zu  ",
                applyMed, applyP95, queryMed, queryP95, queried.size() / 1024.0, sumMed, sumP95, oneReply.size(),
                oneQ.str().size(), setReply.size(), fs1.str().size(), errReply.size(), summary.size(), scene.size());
    CHECK(Reply(errReply).code() == "E_PATH", "the error sample is an error");
}

// ---- main --------------------------------------------------------------------------------

// ---- sub-project 4, step 5: params structs, new methods, the sandbox, cancellation -------

// Every method's params are a documented struct whose schema refuses unknown members.
static void sp4_t08_params_structs_lint_clean_and_close_their_schemas() {
    const auto issues = reflect::lint(api::paramsRegistry());
    for (const auto& i : issues) std::printf("\n      %s", i.c_str());
    CHECK(issues.empty(), "the params registry lints clean (docs, units, registration)");
    for (const auto& m : api::methods()) {
        JsonWriter w;
        reflect::writeParamsSchema(w, *m.params);
        CHECK(w.str().find("\"additionalProperties\":false") != std::string::npos, "closed schemas");
        CHECK(w.str().find("$ref") == std::string::npos, "self-contained schemas");
    }
    World w;
    api::Session s(w, engineTypes());
    const Reply r(s.call("world.step", R"({"frames":2,"framez":3})"));
    CHECK(r.code() == "E_UNKNOWN_FIELD" && r.str("/error/data/0/path") == "/framez" &&
              r.str("/error/data/0/suggestion") == "frames",
          "an unknown param: E_UNKNOWN_FIELD with a suggestion");
    CHECK(Reply(s.call("world.step", R"({"frames":2000000})")).code() == "E_RANGE", "the step cap holds");
    CHECK(Reply(s.call("render.capture", R"({"path":"x.png","mode":"deferd"})")).str("/error/data/0/suggestion") ==
              "deferred",
          "enums suggest");
    CHECK(Reply(s.call("world.apply", "{}")).code() == "E_COMMAND", "required members are required");
    const reflect::JsonDocument meta = parseJson(R"({"frames":1,"_meta":{"progressToken":7}})").value();
    CHECK(s.call("world.step", meta, meta.root()).status == api::CallStatus::Ok, "MCP's _meta is ignored");
    const reflect::JsonDocument bad = parseJson(R"({"frames":"x"})").value();
    CHECK(s.call("world.step", bad, bad.root()).status == api::CallStatus::BadParams, "a params error is BadParams");
    const reflect::JsonDocument missing = parseJson(R"({"name":"nope"})").value();
    CHECK(s.call("world.restore", missing, missing.root()).status == api::CallStatus::Failed,
          "a domain error is Failed");
    CHECK(s.call("world.nope", bad, bad.root()).status == api::CallStatus::UnknownMethod, "and E_METHOD UnknownMethod");
}

static void sp4_engine_info_and_log_read() {
    World w;
    api::Session s(w, engineTypes());
    s.setHostKind(api::HostKind::Headless);
    const Reply info(s.call("engine.info"));
    CHECK(info.str("/result/protocolVersion") == "1" && info.str("/result/schemaHash").size() == 16 &&
              info.str("/result/host") == "headless" && info.str("/result/time/mode") == "lockstep",
          "engine.info: protocol version, schemaHash, host, time");
    CHECK(Reply(s.call("world.info")).str("/result/host") == "headless", "world.info reports the host");

    OHAO_LOG(Warn, api, "api test marker {}", 1);
    const Reply page(s.call("log.read", R"({"level":"warn","channel":"api","limit":1000})"));
    const JsonNode* lines = page.at("/result/lines");
    CHECK(lines && lines->count >= 1, "log.read returns the record");
    const std::string last = "/result/lines/" + std::to_string(lines->count - 1);
    CHECK(page.str(last + "/msg") == "api test marker 1" && page.str(last + "/level") == "warn" &&
              page.str(last + "/channel") == "api",
          "with its level and channel");
    const Reply after(s.call("log.read", "{\"since\":" + page.str("/result/next") + "}"));
    CHECK(after.at("/result/lines") && after.at("/result/lines")->count == 0, "since pages past it");
}

static void sp4_scene_load_replace_clears_and_keeps_snapshots() {
    World w;
    api::Session s(w, engineTypes());
    const std::string path = (kScenes / "v1" / "rooms.scene.json").generic_string();
    CHECK(Reply(s.call("scene.load", "{\"path\":\"" + path + "\"}")).ok(), "first load");
    CHECK(Reply(s.call("world.snapshot", R"({"name":"rooms"})")).ok(), "snapshot");
    CHECK(Reply(s.call("scene.load", "{\"path\":\"" + path + "\"}")).code() == "E_WORLD_NOT_EMPTY",
          "without replace the world must be empty");
    CHECK(Reply(s.call("scene.load", R"({"text":"{\"format\":\"nope\"}","replace":true})")).code() == "E_FORMAT" &&
              w.idCount() == 4,
          "a scene that does not load leaves the world as it was");
    const Reply again(s.call("scene.load", "{\"path\":\"" + path + "\",\"replace\":true}"));
    CHECK(again.ok() && again.str("/result/entities") == "4" && w.idCount() == 4, "replace loads into a cleared world");
    CHECK(Reply(s.call("world.restore", R"({"name":"rooms"})")).ok(), "snapshots survive a replace");
}

// Test 15 (paths part): the sandbox admits project-relative reads, writes under --out,
// and exactly the --allow-path roots.
static void sp4_t15_paths_are_confined_to_the_sandbox() {
    const fs::path root = fs::path(OHAO_SOURCE_DIR) / "build" / ("api_sandbox_" + std::to_string(processId()));
    fs::remove_all(root);
    fs::create_directories(root / "scenes");
    fs::create_directories(root / "allowA");
    fs::create_directories(root / "allowAB");
    fs::copy_file(kScenes / "v1" / "rooms.scene.json", root / "scenes" / "rooms.scene.json");
    fs::copy_file(kScenes / "v1" / "rooms.scene.json", root / "allowAB" / "rooms.scene.json");
    World w;
    api::Session s(w, engineTypes());
    s.setPathPolicy({root, {}, {root / "allowA"}});
    const auto code = [&](const std::string& method, const std::string& params) {
        return Reply(s.call(method, params)).code();
    };
    CHECK(code("scene.validate", R"({"path":"scenes/rooms.scene.json"})").empty(), "a project-relative read");
    CHECK(code("scene.validate", "{\"path\":\"" + (root / "scenes" / "rooms.scene.json").generic_string() + "\"}") ==
              "E_ACCESS",
          "an absolute path outside the allowed roots");
    CHECK(code("scene.validate", R"({"path":"scenes/../scenes/rooms.scene.json"})") == "E_ACCESS", "'..'");
    CHECK(code("scene.validate", R"({"path":"scenes\\rooms.scene.json"})") == "E_ACCESS", "backslashes");
    CHECK(code("scene.load", R"({"path":"scenes/rooms.scene.json"})").empty(), "load");
    CHECK(code("scene.save", R"({"path":"scenes/out.scene.json"})") == "E_ACCESS", "a write outside the output dir");
    CHECK(code("scene.save", R"({"path":"agent_out/sub/out.scene.json"})").empty() &&
              fs::exists(root / "agent_out" / "sub" / "out.scene.json"),
          "a write under the output dir (made on demand)");
    CHECK(code("scene.save", "{\"path\":\"" + (root / "allowA" / "x.scene.json").generic_string() + "\"}").empty(),
          "an absolute write inside an allowed root");
    CHECK(code("scene.validate", "{\"path\":\"" + (root / "allowAB" / "rooms.scene.json").generic_string() +
                                     "\"}") == "E_ACCESS",
          "a sibling that shares the allowed root's prefix is outside it");
    const Reply r(s.call("scene.save", R"({"path":"/etc/passwd"})"));
    CHECK(r.code() == "E_ACCESS" && r.str("/error/data/0/path") == "/path", "E_ACCESS points at the member");
    fs::remove_all(root);
}

// Test 15 (caps part): world.apply takes at most 10,000 commands and input.inject 10,000
// events, counted before any is read; at most 100,000 injected events wait at once; the
// caps are in the params schemas an agent reads.
static void sp4_t15_batch_and_inject_caps_hold() {
    World w;
    api::Session s(w, engineTypes());
    const auto commands = [](uint32_t n) {
        std::string p = R"({"commands":[)";
        for (uint32_t i = 0; i < n; ++i) p += i ? R"(,{"op":"entity.create"})" : R"({"op":"entity.create"})";
        return p + "]}";
    };
    const Reply over(s.call("world.apply", commands(api::kMaxApplyCommands + 1)));
    CHECK(over.code() == "E_RANGE" && over.str("/error/data/0/path") == "/commands" && w.registry().view<ecs::EntityId>().empty(),
          "10,001 commands: E_RANGE at /commands, nothing applied");
    const Reply full(s.call("world.apply", commands(api::kMaxApplyCommands)));
    CHECK(full.code().empty() && full.str("/result/applied") == "10000", "10,000 commands apply");

    const auto events = [](uint32_t n) {
        std::string p = R"({"events":[)";
        for (uint32_t i = 0; i < n; ++i) p += i ? R"(,{"mouse":[1,0]})" : R"({"mouse":[1,0]})";
        return p + "]}";
    };
    const Reply tooMany(s.call("input.inject", events(api::kMaxInjectEvents + 1)));
    CHECK(tooMany.code() == "E_RANGE" && tooMany.str("/error/data/0/path") == "/events",
          "10,001 events in one call: E_RANGE at /events");
    const std::string batch = events(api::kMaxInjectEvents);
    bool queued = true;
    for (std::size_t i = 0; i < api::kMaxQueuedInput / api::kMaxInjectEvents; ++i)
        queued = queued && Reply(s.call("input.inject", batch)).code().empty();
    CHECK(queued, "100,000 events may wait");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"mouse":[1,0]}]})")).code() == "E_STATE",
          "one more is E_STATE until the world steps");
    CHECK(Reply(s.call("world.step", R"({"frames":1})")).code().empty() &&
              Reply(s.call("input.inject", R"({"events":[{"mouse":[1,0]}]})")).code().empty(),
          "a step consumes them; injecting works again");

    reflect::JsonWriter apply, inject;
    reflect::writeParamsSchema(apply, *api::paramsRegistry().find<api::WorldApplyParams>());
    reflect::writeParamsSchema(inject, *api::paramsRegistry().find<api::InputInjectParams>());
    CHECK(apply.str().find("\"maxItems\":10000") != std::string::npos &&
              inject.str().find("\"maxItems\":10000") != std::string::npos,
          "the caps are the schemas' maxItems");
}

// Test 7 (cancellation part): a cancelled world.step stops between whole steps.
static void sp4_t07_cancel_leaves_whole_steps() {
    const std::string spinner =
        R"({"commands":[{"op":"entity.create","name":"Spinner","components":{"Spin":{"degreesPerSecond":37}}}]})";
    World w;
    api::Session s(w, engineTypes());
    CHECK(Reply(s.call("world.apply", spinner)).ok(), "a spinning entity");
    std::atomic<bool> cancel{false};
    uint64_t reported = 0;
    api::CallControl control;
    control.cancel = &cancel;
    control.progress = [&](uint64_t done, uint64_t total) {
        reported = done;
        if (done == 123 && total == 1'000'000) cancel = true;
    };
    const reflect::JsonDocument p = parseJson(R"({"frames":1000000})").value();
    const api::CallOutcome o = s.call("world.step", p, p.root(), &control);
    CHECK(o.status == api::CallStatus::Cancelled && o.errors.empty(), "the call reports Cancelled");
    CHECK(s.frame() == 123 && reported == 123, "after exactly the steps that completed");
    const auto rot = [](World& world) {
        for (auto [e, n, t] : world.registry().view<const Name, const Transform>().each())
            if (n.value == "Spinner") return t.rotation;
        return glm::quat{};
    };
    World fresh;
    api::Session f(fresh, engineTypes());
    CHECK(Reply(f.call("world.apply", spinner)).ok() && Reply(f.call("world.step", R"({"frames":123})")).ok(),
          "a fresh world stepped 123 times");
    const glm::quat a = rot(w), b = rot(fresh);
    CHECK(std::memcmp(&a, &b, sizeof a) == 0, "is bit-identical: no half step");
}

// ---- test 11: the time model ----------------------------------------------------------

static const std::string kSpinners =
    R"({"commands":[{"op":"entity.create","name":"A","components":{"Spin":{"degreesPerSecond":37}}},)"
    R"({"op":"entity.create","name":"B","components":{"Spin":{"axis":[1,0,0],"degreesPerSecond":-90}}},)"
    R"({"op":"entity.create","name":"C","components":{"Transform":{"position":[1,2,3]}}}]})";

static std::string worldHashOf(api::Session& s) { return Reply(s.call("world.hash")).str("/result/hash"); }

// kSpinners in a world whose id generator is seeded, so two such worlds agree on ids.
static bool loadSpinners(World& w, api::Session& s) {
    w.seedIds(0x5eed);
    return Reply(s.call("world.apply", kSpinners)).ok();
}

static void sp4_t11_simulate_runs_once_per_step_and_sync_once_per_batch() {
    World w;
    api::Session s(w, engineTypes());
    int simulated = 0, synced = 0;
    w.addSimulate("count", [&](World&, float) { ++simulated; });
    w.addSync("count", [&](World&) { ++synced; });
    const Reply r(s.call("world.step", R"({"frames":5})"));
    CHECK(r.ok() && r.str("/result/frame") == "5", "five steps");
    CHECK(simulated == 5 && synced == 1, "simulate five times, sync once");
    CHECK(Reply(s.call("world.step", R"({"frames":37})")).ok() && Reply(s.call("world.step", R"({"frames":58})")).ok(),
          "two more batches");
    CHECK(simulated == 100 && synced == 3 && s.frame() == 100, "lockstep counts are exact");
    CHECK(Reply(s.call("time.get")).str("/result/frame") == "100", "time.get reports the frame");
}

static void sp4_t11_world_step_watch_returns_fields_after_the_last_step() {
    World w;
    api::Session s(w, engineTypes());
    CHECK(Reply(s.call("world.apply", kSpinners)).ok(), "spinners");
    const Reply r(s.call("world.step",
                         R"({"frames":3,"watch":{"names":["A","C"],"fields":["/Transform/rotation","/Name"]}})"));
    CHECK(r.ok() && r.str("/result/frame") == "3" && r.str("/result/watch/count") == "2", "two watched entities");
    CHECK(r.str("/result/watch/entities/0/Name/value") == "A" && r.at("/result/watch/entities/0/Transform/rotation"),
          "projected as world.query projects");
    const Reply q(s.call("world.query", R"({"names":["A","C"],"fields":["/Transform/rotation","/Name"]})"));
    CHECK(q.json("/result/entities") == r.json("/result/watch/entities"), "exactly what a world.query after it returns");
    const Reply bad(s.call("world.step", R"({"frames":3,"watch":{"fields":["/Transform/nope"]}})"));
    CHECK(bad.code() == "E_PATH" && bad.str("/error/data/0/path") == "/watch/fields/0", "a bad path points into watch");
    CHECK(s.frame() == 3, "and no step ran");
    CHECK(Reply(s.call("world.step", R"({"watch":{"names":["A"]}})")).code() == "E_COMMAND" && s.frame() == 3,
          "a watch needs fields");
}

static void sp4_t11_time_rules_per_host_and_mode() {
    World w;
    api::Session s(w, engineTypes());
    s.setHostKind(api::HostKind::Headless);
    const Reply t(s.call("time.get"));
    CHECK(t.str("/result/mode") == "lockstep" && t.json("/result/paused") == "false" &&
              t.str("/result/fixedDt") == "0.016666668",
          "a headless session is lockstep at 1/60 s");
    const Reply rt(s.call("time.set", R"({"mode":"realtime"})"));
    CHECK(rt.code() == "E_STATE" && rt.str("/error/data/0/path") == "/mode", "headless hosts are lockstep only");
    CHECK(Reply(s.call("time.set", R"({"fixedDt":0})")).code() == "E_RANGE", "fixedDt must be positive");
    CHECK(Reply(s.call("time.set", R"({"fixedDt":2})")).code() == "E_RANGE", "and at most 1 s");

    // A window host's session: realtime is allowed and stops world.step while it runs.
    World ww;
    api::Session win(ww, engineTypes());
    win.setHostKind(api::HostKind::Window);
    const Reply on(win.call("time.set", R"({"mode":"realtime"})"));
    CHECK(on.ok() && on.str("/result/mode") == "realtime", "realtime on a window host");
    CHECK(Reply(win.call("world.step")).code() == "E_STATE", "world.step while realtime runs is E_STATE");
    CHECK(Reply(win.call("time.set", R"({"paused":true})")).ok() && Reply(win.call("world.step")).ok(),
          "paused, world.step works as in lockstep");
    CHECK(Reply(win.call("time.set", R"({"paused":false})")).ok() && Reply(win.call("world.step")).code() == "E_STATE",
          "resumed, it is E_STATE again");
    CHECK(Reply(win.call("time.set", R"({"mode":"lockstep"})")).ok() && Reply(win.call("world.step")).ok() &&
              win.frame() == 2,
          "lockstep: steps on request, the frame kept throughout");
}

static void sp4_t11_dt_defaults_to_the_fixed_step() {
    World a, b, c;
    api::Session sa(a, engineTypes()), sb(b, engineTypes()), sc(c, engineTypes());
    CHECK(loadSpinners(a, sa) && loadSpinners(b, sb) && loadSpinners(c, sc), "spinners");
    CHECK(Reply(sa.call("world.step", R"({"frames":60})")).ok(), "default dt");
    CHECK(Reply(sb.call("world.step", R"({"frames":60,"dt":0.016666668})")).ok(), "explicit 1/60");
    CHECK(worldHashOf(sa) == worldHashOf(sb), "the default dt is the fixed step");
    CHECK(Reply(sc.call("time.set", R"({"fixedDt":0.05})")).ok() && Reply(sc.call("world.step", R"({"frames":60})")).ok(),
          "fixedDt 0.05, then 60 default steps");
    CHECK(Reply(sb.call("world.step", R"({"frames":0})")).ok() && worldHashOf(sc) != worldHashOf(sb),
          "a different fixed step, a different world");
    World d;
    api::Session sd(d, engineTypes());
    CHECK(loadSpinners(d, sd) && Reply(sd.call("world.step", R"({"frames":60,"dt":0.05})")).ok(), "explicit 0.05");
    CHECK(worldHashOf(sc) == worldHashOf(sd), "follows time.set");
}

// A window host's realtime loop, run by hand: wall time in, whole fixed steps out.
static void sp4_t11_realtime_paused_stepped_reaches_the_lockstep_hash() {
    using namespace std::chrono_literals;
    World w;
    api::Session s(w, engineTypes());
    s.setHostKind(api::HostKind::Window);
    CHECK(loadSpinners(w, s), "spinners");
    CHECK(Reply(s.call("time.set", R"({"mode":"realtime"})")).ok(), "realtime");
    Timeline& t = s.timeline();
    const auto frame = [&](Nanos wall) {   // one host frame: steps, then sync
        const int n = t.advance(wall).steps;
        for (int i = 0; i < n; ++i) {
            w.simulate(t.fixedDt());
            t.stepped(1);
        }
        w.sync();
    };
    const Nanos walls[] = {7'000'000ns, 16'666'667ns, 31'000'000ns, 2'000'000ns, 40'000'000ns, 16'666'667ns};
    for (int i = 0; i < 40; ++i) frame(walls[i % 6]);
    CHECK(Reply(s.call("time.set", R"({"paused":true})")).ok(), "pause");
    for (int i = 0; i < 10; ++i) frame(100'000'000ns);   // paused frames step nothing
    CHECK(Reply(s.call("world.step", R"({"frames":17})")).ok(), "step while paused");
    CHECK(Reply(s.call("time.set", R"({"paused":false})")).ok(), "resume");
    for (int i = 0; i < 25; ++i) frame(walls[(i + 3) % 6]);
    const uint64_t steps = s.frame();
    CHECK(steps > 17, "realtime ran steps");

    World l;
    api::Session ls(l, engineTypes());
    CHECK(loadSpinners(l, ls), "the same scene");
    CHECK(Reply(ls.call("world.step", "{\"frames\":" + std::to_string(steps) + "}")).ok(), "pure lockstep");
    CHECK(worldHashOf(s) == worldHashOf(ls), "the same steps reach the same hash, however they were driven");
    CHECK(Reply(ls.call("world.step")).ok() && worldHashOf(s) != worldHashOf(ls), "one more step does not");
}

// ---- test 12: hash and diff -----------------------------------------------------------

static void sp4_t12_hash_is_stable_across_save_and_load() {
    const std::string path = (kScenes / "v1" / "rooms.scene.json").generic_string();
    const std::string load = "{\"path\":\"" + path + "\",\"seed\":\"00000000000000aa\"}";
    World a;
    api::Session sa(a, engineTypes());
    CHECK(Reply(sa.call("scene.load", load)).ok(), "load");
    const std::string h = worldHashOf(sa);
    CHECK(h.size() == 16 && Reply(sa.call("world.hash")).str("/result/frame") == "0", "a 16-hex-digit hash and the frame");
    CHECK(worldHashOf(sa) == h, "hashing changes nothing");
    const Reply saved(sa.call("scene.save"));
    World b;
    api::Session sb(b, engineTypes());
    CHECK(Reply(sb.call("scene.load", "{\"scene\":" + saved.json("/result/scene") + ",\"seed\":\"00000000000000aa\"}")).ok(),
          "load the save");
    CHECK(worldHashOf(sb) == h, "save -> load with the same seed: the same hash");
    // Runtime state is not authoring state.
    a.sync();   // WorldTransforms and dirty tags
    for (auto [e, n] : a.registry().view<const Name>().each()) a.registry().emplace_or_replace<TransformDirty>(e);
    CHECK(worldHashOf(sa) == h, "runtime components are not hashed");
    World c;
    api::Session sc(c, engineTypes());
    CHECK(Reply(sc.call("scene.load", "{\"path\":\"" + path + "\",\"seed\":\"00000000000000ab\"}")).ok() &&
              worldHashOf(sc) != h,
          "another id seed is another state (it decides future ids)");
}

static void sp4_t12_hash_changes_on_any_authoring_change() {
    World w;
    api::Session s(w, engineTypes());
    CHECK(Reply(s.call("scene.load", "{\"path\":\"" + (kScenes / "v1" / "rooms.scene.json").generic_string() +
                                         "\",\"seed\":\"0000000000000001\"}")).ok(),
          "load");
    CHECK(Reply(s.call("world.snapshot", R"({"name":"base"})")).ok(), "snapshot");
    const std::string base = worldHashOf(s);
    const std::string lamp = std::string("\"") + kLamp + "\"", sun = std::string("\"") + kSun + "\"";
    const std::vector<std::pair<const char*, std::string>> edits = {
        {"a position by one ulp", R"([{"op":"field.set","id":)" + lamp + R"(,"path":"/Transform/position","value":[0,2.0000002,0]}])"},
        {"a sign bit (-0)", R"([{"op":"field.set","id":)" + sun + R"(,"path":"/Transform/position/0","value":-0.0}])"},
        {"a name", R"([{"op":"field.set","id":)" + lamp + R"(,"path":"/Name/value","value":"Lamp2"}])"},
        {"a component added", R"([{"op":"component.set","id":)" + lamp + R"(,"type":"Spin","value":{}}])"},
        {"a component removed", R"([{"op":"component.remove","id":)" + sun + R"(,"type":"Light"}])"},
        {"a reparent", R"([{"op":"entity.reparent","id":)" + sun + R"(,"parent":)" + lamp + "}]"},
        {"an entity created", R"([{"op":"entity.create","name":"New"}])"},
        {"an entity destroyed", R"([{"op":"entity.destroy","id":)" + sun + "}]"},
    };
    for (const auto& [what, commands] : edits) {
        const Reply r = applyBatch(s, commands);
        if (!r.ok()) std::printf("\n      %s: %s", what, r.text.c_str());
        CHECK(r.ok(), what);
        CHECK(worldHashOf(s) != base, what);
        CHECK(Reply(s.call("world.restore", R"({"name":"base"})")).ok() && worldHashOf(s) == base,
              "restored, the hash is back");
    }
}

// The diff a test computes from two saves written with every field: per entity, "/Type"
// for a component in one save only, "/Type/field" for a field whose JSON differs.
struct FileDiff {
    std::map<std::string, std::map<std::string, std::string>> changed;   // id -> path -> "[old,new]" ("" for /Type)
    std::vector<std::string> created, destroyed;
};

static FileDiff diffSaves(const std::string& before, const std::string& after) {
    FileDiff d;
    const auto a = parseJson(before).value(), b = parseJson(after).value();
    const auto entities = [](const JsonDocument& doc) {
        std::map<std::string, const JsonNode*> m;
        const JsonNode& list = *doc.member(doc.root(), "entities");
        for (uint32_t i = 0; i < list.count; ++i) {
            const JsonNode& e = doc.child(list, i);
            m[std::string(doc.member(e, "id")->text)] = &e;
        }
        return m;
    };
    const auto text = [](const JsonDocument& doc, const JsonNode& n) {
        JsonWriter w;
        reflect::writeJson(w, doc, n);
        return w.take();
    };
    const auto ea = entities(a), eb = entities(b);
    for (const auto& [id, na] : ea) {
        const auto it = eb.find(id);
        if (it == eb.end()) {
            d.destroyed.push_back(id);
            continue;
        }
        const JsonNode* nb = it->second;
        std::map<std::string, std::string> paths;
        const auto visit = [&](const JsonDocument& doc, const JsonNode& e, const JsonDocument& other, const JsonNode& oe,
                               bool first) {
            for (uint32_t i = 0; i < e.count; ++i) {
                const JsonNode& comp = doc.child(e, i);
                if (comp.key == "id") continue;
                const JsonNode* oc = other.member(oe, comp.key);
                if (!oc) {
                    paths["/" + std::string(comp.key)] = "";
                    continue;
                }
                if (!first) continue;
                for (uint32_t k = 0; k < comp.count; ++k) {
                    const JsonNode& f = doc.child(comp, k);
                    const JsonNode* of = other.member(*oc, f.key);
                    const std::string ta = text(doc, f), tb = of ? text(other, *of) : "null";
                    if (ta != tb) paths["/" + std::string(comp.key) + "/" + std::string(f.key)] = "[" + ta + "," + tb + "]";
                }
            }
        };
        visit(a, *na, b, *nb, true);
        visit(b, *nb, a, *na, false);
        if (!paths.empty()) d.changed[id] = std::move(paths);
    }
    for (const auto& [id, nb] : eb)
        if (!ea.count(id)) d.created.push_back(id);
    return d;
}

static void sp4_t12_diff_equals_a_diff_of_the_two_saves() {
    World w;
    api::Session s(w, engineTypes());
    buildWorld(w, 12, 200);
    CHECK(Reply(s.call("world.snapshot", R"({"name":"base"})")).ok(), "snapshot");
    const std::string before = Reply(s.call("scene.save", R"({"defaults":true})")).json("/result/scene");
    // Mixed edits, each through the API: positions, names, light fields, components added
    // and removed, reparents, creates, destroys.
    std::mt19937_64 rng(77);
    std::vector<uint64_t> ids = liveIds(w);
    std::shuffle(ids.begin(), ids.end(), rng);
    std::string cmds = "[";
    const auto hex = [](uint64_t id) {
        char b[19];
        std::snprintf(b, sizeof b, "\"%016llx\"", static_cast<unsigned long long>(id));
        return std::string(b);
    };
    for (int i = 0; i < 12; ++i)
        cmds += R"({"op":"field.set","id":)" + hex(ids[i]) + R"(,"path":"/Transform/position/1","value":)" +
                std::to_string(i + 100) + "},";
    for (int i = 12; i < 16; ++i)
        cmds += R"({"op":"field.set","id":)" + hex(ids[i]) + R"(,"path":"/Name/value","value":"renamed )" +
                std::to_string(i) + "\"},";
    for (int i = 16; i < 20; ++i)
        cmds += R"({"op":"component.set","id":)" + hex(ids[i]) + R"(,"type":"Light","value":{"intensity":3}},)";
    int removed = 0;   // four lights removed, from entities not edited above
    for (std::size_t i = 26; i < ids.size() && removed < 4; ++i)
        if (w.registry().all_of<Light>(w.findById(ids[i]))) {
            cmds += R"({"op":"component.remove","id":)" + hex(ids[i]) + R"(,"type":"Light"},)";
            ++removed;
        }
    CHECK(removed == 4, "the world has lights to remove");
    cmds += R"({"op":"entity.reparent","id":)" + hex(ids[24]) + R"(,"parent":null},)";
    cmds += R"({"op":"entity.create","name":"fresh"},)";
    cmds += R"({"op":"entity.destroy","id":)" + hex(ids[25]) + "}]";
    const Reply applied = applyBatch(s, cmds);
    if (!applied.ok()) std::printf("\n      %s", applied.text.c_str());
    CHECK(applied.ok(), "the edits apply");
    const std::string after = Reply(s.call("scene.save", R"({"defaults":true})")).json("/result/scene");
    const FileDiff want = diffSaves(before, after);

    const Reply got(s.call("world.diff", R"({"snapshot":"base","limit":10000})"));
    CHECK(got.ok(), "world.diff");
    const JsonNode* changed = got.at("/result/changed");
    CHECK(changed && got.str("/result/count") == std::to_string(changed->count) && !got.at("/result/more"),
          "count is the number of changed entities; nothing left out");
    std::map<std::string, std::map<std::string, std::string>> have;
    for (uint32_t i = 0; i < changed->count; ++i) {
        const std::string at = "/result/changed/" + std::to_string(i);
        const JsonNode& fields = *got.at(at + "/fields");
        auto& paths = have[got.str(at + "/id")];
        for (uint32_t k = 0; k < fields.count; ++k) {
            const JsonNode& f = got.doc.child(fields, k);
            const std::string path(f.key);
            // Whole components are compared by path: the save holds every field, the diff
            // the file form (defaults left out).
            if (path.find('/', 1) == std::string::npos) {
                paths[path] = "";
                continue;
            }
            JsonWriter jw;   // the keys hold '/', so the value is read through its node
            reflect::writeJson(jw, got.doc, f);
            paths[path] = jw.take();
        }
    }
    CHECK(have == want.changed, "the same entities, paths and [old, new] values as the diff of the two saves");
    const auto list = [&](const char* ptr) {
        std::vector<std::string> v;
        if (const JsonNode* n = got.at(ptr))
            for (uint32_t i = 0; i < n->count; ++i) v.push_back("\"" + std::string(got.doc.child(*n, i).text) + "\"");
        std::sort(v.begin(), v.end());
        return v;
    };
    std::vector<std::string> wc = want.created, wd = want.destroyed;
    std::sort(wc.begin(), wc.end());
    std::sort(wd.begin(), wd.end());
    const auto quoted = [](std::vector<std::string> v) {
        for (auto& x : v) x = "\"" + x + "\"";
        return v;
    };
    CHECK(list("/result/created") == quoted(wc) && list("/result/destroyed") == quoted(wd), "created and destroyed");
    CHECK(Reply(s.call("world.restore", R"({"name":"base"})")).ok(), "restore");
    const Reply none(s.call("world.diff", R"({"snapshot":"base"})"));
    CHECK(none.str("/result/count") == "0" && none.at("/result/created")->count == 0 &&
              none.at("/result/destroyed")->count == 0,
          "restored, nothing differs");
}

static void sp4_t12_diff_respects_limit_fields_and_names() {
    World w;
    api::Session s(w, engineTypes());
    buildWorld(w, 5, 100);
    CHECK(Reply(s.call("world.snapshot", R"({"name":"base"})")).ok(), "snapshot");
    std::string cmds = "[";
    const std::vector<uint64_t> ids = liveIds(w);
    for (int i = 0; i < 30; ++i) {
        char b[19];
        std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(ids[i]));
        cmds += std::string(i ? "," : "") + R"({"op":"field.set","id":")" + b +
                R"(","path":"/Transform/scale","value":[2,2,2]})";
        if (i < 5) cmds += std::string(R"(,{"op":"field.set","id":")") + b + R"(","path":"/Name/value","value":"x"})";
    }
    cmds += R"(,{"op":"entity.create","name":"n1"},{"op":"entity.create","name":"n2"}])";
    CHECK(applyBatch(s, cmds).ok(), "30 scales, 5 names, 2 creates");
    const Reply cut(s.call("world.diff", R"({"snapshot":"base","limit":10})"));
    CHECK(cut.at("/result/changed")->count == 10 && cut.str("/result/count") == "30" && cut.str("/result/more") == "20" &&
              cut.at("/result/created")->count == 2,
          "limit caps each list; count is every changed entity; more says how many were left out");
    const Reply names(s.call("world.diff", R"({"snapshot":"base","fields":["/Name"]})"));
    CHECK(names.str("/result/count") == "5" && names.json("/result/changed/0/fields").find("Transform") == std::string::npos,
          "fields keeps only the listed changes, and only entities that have one");
    const Reply scale(s.call("world.diff", R"({"snapshot":"base","fields":["/Transform/position"]})"));
    CHECK(scale.str("/result/count") == "0", "a field that did not change lists nothing");
    CHECK(Reply(s.call("world.diff", R"({"snapshot":"nope"})")).code() == "E_SNAPSHOT_UNKNOWN", "an unknown snapshot");
    const Reply bad(s.call("world.diff", R"({"snapshot":"base","fields":["/Nope"]})"));
    CHECK(bad.code() == "E_COMPONENT_UNKNOWN" && bad.str("/error/data/0/path") == "/fields/0", "a bad projection");
    CHECK(Reply(s.call("world.diff", "{}")).code() == "E_COMMAND", "snapshot is required");
}

// ---- test 13: input as data -----------------------------------------------------------

// A world with controls (fire on the left mouse button, move_forward on W/S, look on the
// right button) and a fly camera, its id generator seeded.
static const std::string kControls =
    R"({"commands":[{"op":"entity.create","name":"Controls","components":{"InputActions":{)"
    R"("actions":[{"action":"fire","key":"MouseLeft"},{"action":"look","key":"MouseRight"},{"action":"fast","key":"LeftShift"}],)"
    R"("axes":[{"axis":"move_forward","positive":"W","negative":"S"},{"axis":"move_right","positive":"D","negative":"A"}]}}},)"
    R"({"op":"entity.create","name":"Eye","components":{"FlyController":{},"Transform":{"position":[0,1,5]}}}]})";

static bool loadControls(World& w, api::Session& s) {
    w.seedIds(0xc0de);
    return Reply(s.call("world.apply", kControls)).ok();
}

static bool step1(api::Session& s) { return Reply(s.call("world.step")).ok(); }

static void sp4_t13_injection_is_stamped_by_step() {
    World w;
    api::Session s(w, engineTypes());
    CHECK(loadControls(w, s), "controls");
    const Reply q(s.call("input.inject", R"({"events":[{"step":2,"action":"fire","down":true},{"step":5,"action":"fire","down":false}]})"));
    CHECK(q.ok() && q.str("/result/queued") == "2" && q.str("/result/firstStep") == "2" && q.str("/result/lastStep") == "5",
          "two events queued for absolute steps 2 and 5");
    const auto fire = [&](const char* what) { return Reply(s.call("input.state")).json(std::string("/result/actions/fire/") + what); };
    CHECK(step1(s) && step1(s) && fire("held") == "false", "steps 0 and 1: not yet");
    CHECK(step1(s) && fire("pressed") == "true" && fire("held") == "true", "step 2: pressed, exactly on its step");
    CHECK(step1(s) && fire("pressed") == "false" && fire("held") == "true", "step 3: held, no longer pressed");
    CHECK(step1(s) && fire("held") == "true", "step 4: still held");
    CHECK(step1(s) && fire("released") == "true" && fire("held") == "false", "step 5: released on the release step");
    const Reply past(s.call("input.inject", R"({"events":[{"step":-1,"action":"fire","down":true}]})"));
    CHECK(past.code() == "E_RANGE" && past.str("/error/data/0/path") == "/events/0/step", "no event in the past");
}

static void sp4_t13_axis_override_holds_and_releases_to_the_keys() {
    World w;
    api::Session s(w, engineTypes());
    CHECK(loadControls(w, s), "controls");
    const auto axis = [&] { return Reply(s.call("input.state")).str("/result/axes/move_forward"); };
    CHECK(Reply(s.call("input.inject", R"({"events":[{"key":"W","down":true}]})")).ok() && step1(s) && axis() == "1",
          "a key goes through the bindings like a real key");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"axis":"move_forward","value":-0.5}]})")).ok() && step1(s) &&
              axis() == "-0.5",
          "an injected value overrides the keys");
    CHECK(step1(s) && step1(s) && axis() == "-0.5", "and holds until changed");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"axis":"move_forward","value":null}]})")).ok() && step1(s) &&
              axis() == "1",
          "value null releases it back to its keys");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"key":"W","down":false}]})")).ok() && step1(s) && axis() == "0",
          "key up");
    const Reply bad(s.call("input.inject", R"({"events":[{"axis":"move_forward"}]})"));
    CHECK(bad.code() == "E_COMMAND" && bad.str("/error/data/0/path") == "/events/0", "an axis event needs a value");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"action":"fire","key":"W","down":true}]})")).code() == "E_COMMAND",
          "exactly one kind per event");
}

static void sp4_t13_input_actions_rebind_on_change() {
    World w;
    api::Session s(w, engineTypes());
    CHECK(loadControls(w, s), "controls");
    const auto fireHeld = [&] { return Reply(s.call("input.state")).json("/result/actions/fire/held"); };
    CHECK(Reply(s.call("input.inject", R"({"events":[{"key":"MouseLeft","down":true}]})")).ok() && step1(s) &&
              fireHeld() == "true",
          "fire on the left button");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"key":"MouseLeft","down":false}]})")).ok() && step1(s), "let go");
    const std::string controls = Reply(s.call("world.query", R"({"names":["Controls"],"fields":[]})")).str("/result/entities/0/id");
    const Reply set = applyBatch(s, R"([{"op":"field.set","id":")" + controls +
                                        R"(","path":"/InputActions/actions/0/key","value":"Space"}])");
    CHECK(set.ok(), "rebind fire to Space through a command");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"key":"MouseLeft","down":true}]})")).ok() && step1(s) &&
              fireHeld() == "false",
          "the old binding is gone at the next step");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"key":"Space","down":true}]})")).ok() && step1(s) &&
              fireHeld() == "true",
          "the new one holds fire");
    CHECK(applyBatch(s, R"([{"op":"component.remove","id":")" + controls + R"(","type":"InputActions"}])").ok() &&
              step1(s) && fireHeld() == "false",
          "removing InputActions drops every binding");
}

static void sp4_t13_focus_lost_releases_everything() {
    World w;
    api::Session s(w, engineTypes());
    CHECK(loadControls(w, s), "controls");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"key":"W","down":true},{"action":"look","down":true},)"
                                         R"({"axis":"move_right","value":0.25},{"key":"MouseLeft","down":true}]})")).ok() &&
              step1(s),
          "a key, an injected action, an axis value, a button");
    const Reply held(s.call("input.state"));
    CHECK(held.str("/result/axes/move_forward") == "1" && held.json("/result/actions/look/held") == "true" &&
              held.str("/result/axes/move_right") == "0.25" && held.json("/result/actions/fire/held") == "true",
          "all held");
    CHECK(Reply(s.call("input.inject", R"({"events":[{"focusLost":true}]})")).ok() && step1(s), "focus lost");
    const Reply after(s.call("input.state"));
    CHECK(after.str("/result/axes/move_forward") == "0" && after.json("/result/actions/look/released") == "true" &&
              after.str("/result/axes/move_right") == "0" && after.json("/result/actions/fire/held") == "false",
          "everything released, real and injected");
}

static void sp4_t13_unknown_key_names_are_e_enum_with_a_suggestion() {
    World w;
    api::Session s(w, engineTypes());
    const Reply cmd(s.call("input.inject", R"({"events":[{"key":"Spcae","down":true}]})"));
    CHECK(cmd.code() == "E_ENUM" && cmd.str("/error/data/0/suggestion") == "Space" &&
              cmd.str("/error/data/0/path") == "/events/0/key",
          "in commands");
    const Reply file(s.call("scene.validate", R"({"text":"{\"format\":\"ohao.scene\",\"version\":1,\"entities\":[{\"id\":\"00000000000000a1\",)"
                                              R"(\"InputActions\":{\"actions\":[{\"action\":\"run\",\"key\":\"LeftShit\"}]}}]}"})"));
    CHECK(file.json("/result/ok") == "false" && file.str("/result/errors/0/code") == "E_ENUM" &&
              file.str("/result/errors/0/suggestion") == "LeftShift",
          "in files");
}

// A flight: fly forward, turn with the mouse while looking, strafe, stop.
static const std::string kFlight =
    R"({"events":[{"step":0,"axis":"move_forward","value":1},{"step":30,"action":"look","down":true},)"
    R"({"step":30,"mouse":[12,-4]},{"step":31,"mouse":[3,0]},{"step":40,"action":"look","down":false},)"
    R"({"step":70,"key":"D","down":true},{"step":95,"key":"D","down":false},{"step":110,"axis":"move_forward","value":null}]})";

static std::string tmpDir(const char* name) {
    const fs::path d = fs::path(OHAO_SOURCE_DIR) / "build" / (std::string(name) + "_" + std::to_string(processId()));
    fs::create_directories(d);
    return d.generic_string();
}

static void sp4_t13_record_then_replay_matches_every_checkpoint() {
    const std::string dir = tmpDir("api_input");
    World w;
    api::Session s(w, engineTypes());
    CHECK(loadControls(w, s) && Reply(s.call("world.snapshot", R"({"name":"start"})")).ok(), "controls, snapshot");
    const std::string path = dir + "/flight.input.jsonl";
    CHECK(Reply(s.call("input.record", "{\"path\":\"" + path + "\"}")).ok(), "record");
    CHECK(Reply(s.call("input.inject", kFlight)).ok() && Reply(s.call("world.step", R"({"frames":150})")).ok(), "fly 150 steps");
    const std::string flown = worldHashOf(s);
    const Reply stop(s.call("input.record", R"({"stop":true})"));
    CHECK(stop.ok() && stop.str("/result/steps") == "150" && stop.str("/result/events") == "8", "150 steps, 8 events");
    const std::string eye = Reply(s.call("world.query", R"({"names":["Eye"],"fields":["/Transform/position"]})")).json("/result/entities/0");
    CHECK(Reply(s.call("world.restore", R"({"name":"start"})")).ok() && worldHashOf(s) != flown, "back at the start");
    const Reply replay(s.call("input.replay", "{\"path\":\"" + path + "\"}"));
    CHECK(replay.ok() && replay.str("/result/steps") == "150" && replay.str("/result/events") == "8" &&
              replay.str("/result/checkpoints") == "3",
          "150 steps, 8 events, checkpoints at 60, 120 and the end");
    CHECK(worldHashOf(s) == flown, "the replay reaches the recorded world");
    CHECK(Reply(s.call("world.query", R"({"names":["Eye"],"fields":["/Transform/position"]})")).json("/result/entities/0") == eye &&
              eye.find("[0,1,5]") == std::string::npos,
          "the camera flew, and the replay flew it to the same place");
    const Reply wrong(s.call("input.replay", "{\"path\":\"" + path + "\"}"));
    CHECK(wrong.code() == "E_REPLAY" && wrong.str("/error/message").find("does not match the recording's start") != std::string::npos,
          "from another world it refuses to start");
    CHECK(Reply(s.call("input.replay", "{\"path\":\"" + path + "\",\"verify\":false}")).ok(), "unless verify is false");
}

static void sp4_t13_a_tampered_recording_fails_at_the_right_step() {
    const std::string dir = tmpDir("api_input_tamper");
    World w;
    api::Session s(w, engineTypes());
    CHECK(loadControls(w, s) && Reply(s.call("world.snapshot", R"({"name":"start"})")).ok(), "controls, snapshot");
    const std::string path = dir + "/flight.input.jsonl";
    CHECK(Reply(s.call("input.record", "{\"path\":\"" + path + "\"}")).ok() && Reply(s.call("input.inject", kFlight)).ok() &&
              Reply(s.call("world.step", R"({"frames":150})")).ok() && Reply(s.call("input.record", R"({"stop":true})")).ok(),
          "record a flight");
    std::string text = readFile(path);
    const std::string from = R"({"step":70,"key":"D","down":true})";
    const auto at = text.find(from);
    CHECK(at != std::string::npos, "the strafe is in the recording");
    text.replace(at, from.size(), R"({"step":70,"key":"A","down":true})");   // strafe left instead
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    CHECK(Reply(s.call("world.restore", R"({"name":"start"})")).ok(), "restore");
    const Reply r(s.call("input.replay", "{\"path\":\"" + path + "\"}"));
    CHECK(r.code() == "E_REPLAY" && r.str("/error/message").find("at step 120") != std::string::npos,
          "fails at the first checkpoint after the change (step 120), not before");
    CHECK(s.frame() == 120, "having run exactly the steps before it");
}

// A realtime session recorded by hand (the window host's loop), with a long stall the clock
// clamps (dropped time), replays exactly in lockstep.
static void sp4_t13_a_realtime_recording_with_dropped_time_replays_exactly() {
    using namespace std::chrono_literals;
    const std::string dir = tmpDir("api_input_realtime");
    World w;
    api::Session s(w, engineTypes());
    s.setHostKind(api::HostKind::Window);
    CHECK(loadControls(w, s) && Reply(s.call("world.snapshot", R"({"name":"start"})")).ok(), "controls, snapshot");
    const std::string path = dir + "/realtime.input.jsonl";
    CHECK(Reply(s.call("input.record", "{\"path\":\"" + path + "\"}")).ok(), "record");
    CHECK(Reply(s.call("time.set", R"({"mode":"realtime"})")).ok(), "realtime");
    Timeline& t = s.timeline();
    const InputEvent wDown{InputEvent::Type::Down, InputDevice::Key, 87}, wUp{InputEvent::Type::Up, InputDevice::Key, 87};
    std::vector<InputEvent> pending;
    const Nanos walls[] = {16'666'667ns, 9'000'000ns, 2'000'000'000ns /* a stall: clamped */, 33'000'000ns, 16'666'667ns};
    std::size_t clamped = 0;
    for (int frame = 0; frame < 30; ++frame) {
        if (frame == 2) pending.push_back(wDown);   // device events arrive between frames
        if (frame == 20) pending.push_back(wUp);
        const Tick tick = t.advance(walls[frame % 5]);
        clamped += tick.discarded.count() > 0;
        for (int i = 0; i < tick.steps; ++i) {
            s.step(pending, t.fixedDt());   // the step that consumes them
            pending.clear();
        }
        w.sync();
    }
    CHECK(clamped > 0, "the stall dropped time");
    CHECK(Reply(s.call("time.set", R"({"paused":true})")).ok(), "pause");
    const std::string played = worldHashOf(s);
    const Reply stop(s.call("input.record", R"({"stop":true})"));
    CHECK(stop.ok() && stop.str("/result/events") == "2", "both key events recorded");
    CHECK(Reply(s.call("time.set", R"({"mode":"lockstep","paused":false})")).ok() &&
              Reply(s.call("world.restore", R"({"name":"start"})")).ok(),
          "lockstep, back at the start");
    const Reply r(s.call("input.replay", "{\"path\":\"" + path + "\"}"));
    CHECK(r.ok(), "replays");
    CHECK(worldHashOf(s) == played, "to the world the realtime session reached");
}

// ---- sub-project 5, test 8: the level and physics surface ---------------------------------------
// level.info, level.reload, physics.raycast and physics.info on a session whose world has the
// level and physics systems, as the CLI context and the window host install them.

struct LevelWorld {
    FileAssetResolver files{OHAO_SOURCE_DIR};   // outlives the world's systems
    World w;
    api::Session s{w, engineTypes()};
    LevelWorld() {
        ohao::level::installLevels(w, files, engineTypes());
        ecs::installPhysics(w, files);
        s.setAssetResolver(&files);
    }
    Reply load(const char* scene = "assets/scenes/blockout_room.scene.json") {
        const std::string path = (fs::path(OHAO_SOURCE_DIR) / scene).generic_string();
        return Reply(s.call("scene.load", "{\"path\":\"" + path + "\"}"));
    }
};

constexpr const char* kBlockoutLevel = "5c0b10c000000005";   // the blockout scene's Level entity

static void sp5_t08_level_info_counts_and_spawns() {
    LevelWorld lw;
    CHECK(Reply(lw.s.call("level.info")).code() == "E_NOT_FOUND", "no level in the world: E_NOT_FOUND");
    CHECK(lw.load().ok(), "the blockout scene loads");
    const Reply r(lw.s.call("level.info"));
    std::printf("\n      %s", r.text.c_str());
    CHECK(r.ok() && r.str("/result/entity") == kBlockoutLevel && r.str("/result/path") == "assets/levels/blockout_room.glb",
          "the only level, by id and file");
    CHECK(r.str("/result/nodes") == "18" && r.str("/result/meshes") == "14" && r.str("/result/uniqueMeshes") == "14" &&
              r.str("/result/triangles") == "164" && r.str("/result/lights") == "0" && r.str("/result/cameras") == "1",
          "18 nodes, 14 meshes (14 unique, 164 triangles), no light, one camera");
    CHECK(r.json("/result/colliders") == R"({"box":13,"mesh":1})" && r.json("/result/bodies") == R"({"static":12,"dynamic":2})",
          "13 box colliders (the outdoor Ground's too) and the ramp's mesh; 12 static bodies and the 2 crates");
    CHECK(r.at("/result/spawns") && r.at("/result/spawns")->count == 3, "three spawns");
    // Node order is the file's (Blender writes its objects by name): the enemies, then the player.
    CHECK(r.str("/result/spawns/0/tag") == "enemy" && r.json("/result/spawns/0/pos") == "[-2.5,0.3,-2]" &&
              r.str("/result/spawns/0/yawDeg") == "180" && r.str("/result/spawns/1/tag") == "enemy" &&
              r.json("/result/spawns/1/pos") == "[3,0.5,-2]" && r.str("/result/spawns/1/yawDeg") == "180",
          "the enemies in node order, at their authored places, facing +Z");
    CHECK(r.str("/result/spawns/2/tag") == "player" && r.json("/result/spawns/2/pos") == "[1.5,0,2.2]" &&
              r.str("/result/spawns/2/yawDeg") == "0",
          "then the player, at (1.5, 0, 2.2), facing -Z");
    const uint64_t player = std::stoull(r.str("/result/spawns/2/entity"), nullptr, 16);
    const entt::entity pe = lw.w.findById(player);
    CHECK(pe != entt::null && lw.w.registry().get<Name>(pe).value == "SpawnPlayer", "the spawn's entity is its node");
    CHECK(r.at("/result/warnings") && r.at("/result/warnings")->count == 0, "no warnings");
    CHECK(r.at("/result/loadMs") && std::stod(r.str("/result/loadMs")) > 0.0, "and its load time");
    const Reply byId(lw.s.call("level.info", "{\"entity\":\"" + std::string(kBlockoutLevel) + "\"}"));
    CHECK(byId.ok() && byId.json("/result/spawns") == r.json("/result/spawns"), "named by entity, the same level");
    const Reply sun(lw.s.call("level.info", R"({"entity":"5c0b10c000000003"})"));
    CHECK(sun.code() == "E_NOT_FOUND" && sun.str("/error/data/0/path") == "/entity", "a light is not a level: E_NOT_FOUND");
    CHECK(Reply(lw.s.call("level.info", R"({"entity":"00000000000000ff"})")).code() == "E_NOT_FOUND",
          "nor is an id no entity has");
}

static void sp5_t08_level_info_is_small() {
    LevelWorld lw;
    CHECK(lw.load().ok(), "the blockout scene loads");
    const Reply r(lw.s.call("level.info"));
    const std::string result = r.json("/result");
    std::printf("\n      level.info: %zu bytes", result.size());
    CHECK(r.ok() && result.size() <= 1024, "level.info on the blockout is at most 1,024 bytes (spec, token budget)");
}

static void sp5_t08_raycast_hits_floor() {
    LevelWorld lw;
    CHECK(lw.load().ok(), "the blockout scene loads");
    // Right after scene.load: the method builds the bodies it needs (no step, no render first).
    const Reply r(lw.s.call("physics.raycast", R"({"from":[1.5,0.5,2.2],"dir":[0,-1,0],"max":2})"));
    const std::string result = r.json("/result");
    std::printf("\n      %s (%zu bytes)", result.c_str(), result.size());
    CHECK(r.ok() && r.json("/result/hit") == "true" && r.str("/result/name") == "Floor", "a ray down at the player spawn hits the Floor");
    CHECK(r.json("/result/point") == "[1.5,0,2.2]" && r.json("/result/normal") == "[0,1,0]" && r.str("/result/dist") == "0.5",
          "at the spawn's height, facing up, 0.5 m down");
    const entt::entity floor = lw.w.findById(std::stoull(r.str("/result/entity"), nullptr, 16));
    CHECK(floor != entt::null && lw.w.registry().get<Name>(floor).value == "Floor", "naming the Floor's entity");
    CHECK(result.size() <= 200, "at most 200 bytes (spec, token budget)");
    const Reply miss(lw.s.call("physics.raycast", R"({"from":[1.5,1,2.5],"dir":[0,0,1],"max":3})"));
    CHECK(miss.ok() && miss.json("/result") == R"({"hit":false})", "through the doorway: {hit: false}");
    const Reply far(lw.s.call("physics.raycast", R"({"from":[0.6,1,2.5],"dir":[0,0,2]})"));
    CHECK(far.ok() && far.str("/result/name") == "WallSouthA" && far.json("/result/point") == "[0.6,1,3]",
          "max defaults to 1000; dir need not be unit length: beside the doorway, WallSouthA at z 3");
}

static void sp5_t08_raycast_params_refused() {
    {
        World w;
        api::Session s(w, engineTypes());
        CHECK(Reply(s.call("physics.raycast", R"({"from":[0,1,0],"dir":[0,-1,0]})")).code() == "E_STATE",
              "a world without the physics system: E_STATE");
        CHECK(Reply(s.call("physics.info")).code() == "E_STATE", "physics.info too");
        CHECK(Reply(s.call("level.info")).code() == "E_STATE" && Reply(s.call("level.reload")).code() == "E_STATE",
              "and level.info and level.reload without the level system");
    }
    LevelWorld lw;
    CHECK(lw.load().ok(), "the blockout scene loads");
    const Reply zero(lw.s.call("physics.raycast", R"({"from":[0,1,0],"dir":[0,0,0]})"));
    CHECK(zero.code() == "E_PARAMS" && zero.str("/error/data/0/path") == "/dir", "a zero direction: E_PARAMS at /dir");
    for (const char* max : {"0", "-1", "100000.5", "1e9"}) {
        const Reply r(lw.s.call("physics.raycast", std::string(R"({"from":[0,1,0],"dir":[0,-1,0],"max":)") + max + "}"));
        CHECK(r.code() == "E_PARAMS" && r.str("/error/data/0/path") == "/max", "max outside (0, 100000]: E_PARAMS at /max");
    }
    CHECK(Reply(lw.s.call("physics.raycast", R"({"from":[0,1,0],"dir":[0,-1,0],"max":100000})")).ok(),
          "max 100000 is allowed");
    CHECK(Reply(lw.s.call("physics.raycast", R"({"dir":[0,-1,0]})")).code() == "E_COMMAND", "from is required");
    CHECK(Reply(lw.s.call("physics.raycast", R"({"from":[0,1],"dir":[0,-1,0]})")).code() == "E_TYPE",
          "from is three numbers");
    CHECK(Reply(lw.s.call("physics.raycast", R"({"from":[0,1,0],"dir":[0,-1,0],"mx":5})")).code() == "E_UNKNOWN_FIELD",
          "an unknown member is refused");
}

static void sp5_t08_physics_info_counts() {
    LevelWorld lw;
    const Reply empty(lw.s.call("physics.info"));
    CHECK(empty.ok() && empty.json("/result/bodies") == "{}" && empty.str("/result/steps") == "0",
          "an empty world: no bodies");
    CHECK(lw.load().ok(), "the blockout scene loads");
    const Reply r(lw.s.call("physics.info"));
    std::printf("\n      %s", r.text.c_str());
    CHECK(r.ok() && r.json("/result/bodies") == R"({"static":12,"dynamic":2})" &&
              r.json("/result/shapes") == R"({"box":13,"mesh":1})" && r.str("/result/steps") == "0",
          "the blockout: 12 static bodies and 2 dynamic; 13 boxes and a mesh; nothing stepped yet");
    CHECK(r.at("/result/buildMs") && !r.at("/result/warnings"), "its build time; no warnings");
}

// ---- sub-project 6, step 1: physics in world.step, physics.events -------------------------------

// The blockout loaded, its crates lifted (CrateA 1 m, CrateB 2 m) by world.apply as an agent would.
static void liftBlockoutCrates(LevelWorld& lw) {
    std::string commands;
    for (const auto& [name, y] : {std::pair{"CrateA", 1.3f}, std::pair{"CrateB", 2.8f}}) {
        entt::entity e = entt::null;
        for (auto [ent, n] : lw.w.registry().view<const Name>().each())
            if (n.value == name) e = ent;
        char line[200];
        std::snprintf(line, sizeof line, R"(%s{"op":"field.set","id":"%016llx","path":"/Transform/position","value":[1.3,%g,-1.7]})",
                      commands.empty() ? "" : ",", static_cast<unsigned long long>(lw.w.idOf(e)), y);
        commands += line;
    }
    CHECK(Reply(lw.s.call("world.apply", "{\"commands\":[" + commands + "]}")).ok(), "world.apply lifts the crates");
}

static void sp6_t11_physics_events_after_drop() {
    LevelWorld lw;
    CHECK(lw.load().ok(), "the blockout scene loads");
    liftBlockoutCrates(lw);
    CHECK(Reply(lw.s.call("world.step", R"({"frames":60})")).ok(), "60 steps");
    const Reply r(lw.s.call("physics.events"));
    std::printf("\n      %.400s", r.text.c_str());
    CHECK(r.ok() && r.at("/result/events") && r.at("/result/events")->count >= 2, "the drop's events");
    CHECK(r.str("/result/events/0/kind") == "contact_begin" &&
              ((r.str("/result/events/0/an") == "CrateA" && r.str("/result/events/0/bn") == "Floor") ||
               (r.str("/result/events/0/an") == "Floor" && r.str("/result/events/0/bn") == "CrateA")),
          "first CrateA lands on the Floor");
    const int step0 = std::stoi(r.str("/result/events/0/step"));
    CHECK(step0 >= 26 && step0 <= 29 && r.str("/result/events/0/seq") == "1", "at step 26-29, seq 1");
    std::size_t largest = 0;
    for (uint32_t i = 0; i < r.at("/result/events")->count; ++i)
        largest = std::max(largest, r.json("/result/events/" + std::to_string(i)).size());
    std::printf("\n      largest event %zu bytes", largest);
    CHECK(largest < 200, "every event is under 200 bytes");
    const Reply info(lw.s.call("physics.info"));
    CHECK(info.str("/result/steps") == "60" && std::stoi(info.str("/result/contacts")) >= 2, "60 steps; the crates are in contact");
    const Reply q(lw.s.call("world.query", R"({"names":["CrateA"],"fields":["/Transform/position"]})"));
    std::printf("\n      %s", q.json("/result").c_str());
    const std::string y = q.str("/result/entities/0/Transform/position/1");
    CHECK(q.ok() && !y.empty() && std::fabs(std::stod(y) - 0.3) < 0.01, "CrateA's Transform rests at y 0.3 (1 cm; agents see it)");
    CHECK(r.text.find("-0,") == std::string::npos && r.text.find("-0]") == std::string::npos, "no -0 in the events");
}

static void sp6_t11_events_cursor_and_dropped() {
    LevelWorld lw;
    CHECK(lw.load().ok(), "the blockout scene loads");
    liftBlockoutCrates(lw);
    CHECK(Reply(lw.s.call("world.step", R"({"frames":60})")).ok(), "60 steps");
    const Reply all(lw.s.call("physics.events", R"({"max":64})"));
    const std::string next = all.str("/result/next");
    const std::size_t count = all.at("/result/events")->count;
    CHECK(next == std::to_string(count), "next is the last event's seq");
    const Reply none(lw.s.call("physics.events", "{\"since\":" + next + "}"));
    CHECK(none.ok() && none.at("/result/events")->count == 0 && none.str("/result/next") == next && !none.at("/result/dropped"),
          "nothing after next; next stays");
    const Reply one(lw.s.call("physics.events", R"({"since":1,"max":1})"));
    CHECK(one.ok() && one.at("/result/events")->count == 1 && one.str("/result/events/0/seq") == "2" &&
              one.str("/result/next") == "2",
          "since 1, max 1: event 2, next 2");
    const Reply ahead(lw.s.call("physics.events", R"({"since":100000})"));
    CHECK(ahead.ok() && ahead.str("/result/next") == next, "a cursor past the log comes back as the log's end");
}

static void sp6_t11_events_params_refused() {
    {
        World w;
        api::Session s(w, engineTypes());
        CHECK(Reply(s.call("physics.events")).code() == "E_STATE", "a world without the physics system: E_STATE");
    }
    LevelWorld lw;
    for (const char* bad : {R"({"max":0})", R"({"max":65})"}) {
        const Reply r(lw.s.call("physics.events", bad));
        CHECK(r.code() == "E_RANGE" && r.str("/error/data/0/path") == "/max", "max outside 1..64: E_RANGE at /max");
    }
    CHECK(Reply(lw.s.call("physics.events", R"({"since":-1})")).code() == "E_RANGE", "since below 0: E_RANGE");
    CHECK(Reply(lw.s.call("physics.events", R"({"sinse":1})")).code() == "E_UNKNOWN_FIELD", "an unknown member is refused");
    const Reply empty(lw.s.call("physics.events"));
    CHECK(empty.ok() && empty.json("/result") == R"({"events":[],"next":0})", "no events yet: {events: [], next: 0}");
}

static void sp6_t11_physics_info_counts_steps() {
    LevelWorld lw;
    CHECK(lw.load().ok(), "the blockout scene loads");
    CHECK(Reply(lw.s.call("world.step", R"({"frames":5})")).ok(), "5 steps");
    const Reply r(lw.s.call("physics.info"));
    std::printf("\n      %s", r.text.c_str());
    CHECK(r.ok() && r.str("/result/steps") == "5", "physics.info counts the steps world.step ran");
    CHECK(r.at("/result/awake") && r.at("/result/contacts") && !r.at("/result/triggers"), "awake and contacts; no triggers");
}

// ---- sub-project 6, step 4: footsteps in physics.events -----------------------------------------

static void sp6_t13_footsteps_in_physics_events() {
    LevelWorld lw;
    CHECK(lw.load("assets/scenes/blockout_play.scene.json").ok(), "the play scene loads");
    CHECK(Reply(lw.s.call("input.inject", R"({"events":[{"step":0,"axis":"move_forward","value":1}]})")).ok(),
          "walk forward");
    CHECK(Reply(lw.s.call("world.step", R"({"frames":40})")).ok(), "40 steps");
    const Reply r(lw.s.call("physics.events", R"({"max":64})"));
    std::printf("\n      %.600s", r.text.c_str());
    int footsteps = 0;
    std::size_t largest = 0;
    bool shape = true;
    for (uint32_t i = 0; r.at("/result/events") && i < r.at("/result/events")->count; ++i) {
        const std::string at = "/result/events/" + std::to_string(i);
        largest = std::max(largest, r.json(at).size());
        if (r.str(at + "/kind") != "footstep") continue;
        ++footsteps;
        shape &= r.str(at + "/an") == "Player" && r.str(at + "/bn") == "Floor" && r.str(at + "/surface") == "concrete" &&
                 r.at(at + "/point") && r.at(at + "/speed");
    }
    CHECK(r.ok() && footsteps >= 2, "the walk's footsteps are in the log (2.3 m: three strides)");
    CHECK(shape, "each names the player and the Floor, with its surface, point and speed");
    CHECK(largest < 200, "every event under 200 bytes");
}

// ---- sub-project 6, step 5: player.info, player.teleport, physics.contacts, replay ----------------
// docs/specs/2026-10-26-player-physics-design.md, "The agent-facing surface" and "Snapshots and
// replay". The play scene's player is 5c0b1a7e00000001, its head 5c0b1a7e00000002.

constexpr const char* kPlayer = "5c0b1a7e00000001";
constexpr const char* kPlayScene = "assets/scenes/blockout_play.scene.json";

static void sp6_t14_player_info_after_load_and_walk() {
    {
        World w;
        api::Session s(w, engineTypes());
        CHECK(Reply(s.call("player.info")).code() == "E_STATE", "a world without the physics system: E_STATE");
    }
    {
        LevelWorld lw;
        CHECK(lw.load().ok(), "the blockout scene (no player) loads");
        const Reply none(lw.s.call("player.info"));
        CHECK(none.code() == "E_STATE", "a world without a player: E_STATE");
    }
    LevelWorld lw;
    CHECK(lw.load(kPlayScene).ok(), "the play scene loads");
    const Reply r(lw.s.call("player.info"));
    std::printf("\n      %s (%zu bytes)", r.json("/result").c_str(), r.json("/result").size());
    CHECK(r.ok() && r.str("/result/entity") == kPlayer && r.str("/result/name") == "Player", "the only player, by id and name");
    CHECK(r.json("/result/pos") == "[1.5,0,2.2]" && r.str("/result/yawDeg") == "0" && r.str("/result/pitchDeg") == "0" &&
              r.json("/result/velocity") == "[0,0,0]",
          "at the spawn, facing -Z, still, before any step");
    CHECK(r.json("/result/ground") == "true" && r.str("/result/on") == "Floor" && r.str("/result/surface") == "concrete" &&
              r.str("/result/footsteps") == "0",
          "standing on the Floor's concrete, no footstep yet");
    CHECK(r.json("/result").size() < 300, "under 300 bytes");

    CHECK(Reply(lw.s.call("input.inject", R"({"events":[{"axis":"move_forward","value":1}]})")).ok() &&
              Reply(lw.s.call("world.step", R"({"frames":30})")).ok(),
          "walk forward 30 steps");
    const Reply w(lw.s.call("player.info"));
    std::printf("\n      %s", w.json("/result").c_str());
    const std::string z = w.str("/result/pos/2"), vz = w.str("/result/velocity/2");
    CHECK(w.ok() && !z.empty() && std::stod(z) < 0.6 && std::stod(z) > 0.1, "30 steps north: about 1.9 m from z 2.2");
    CHECK(!vz.empty() && std::fabs(std::stod(vz) + 4.0) < 0.01 && w.json("/result/ground") == "true",
          "at the walk speed, 4 m/s toward -Z, on the ground");
    CHECK(std::stoi(w.str("/result/footsteps")) >= 2, "and its footsteps counted");

    const Reply head(lw.s.call("player.info", R"({"entity":"5c0b1a7e00000002"})"));
    CHECK(head.code() == "E_NOT_FOUND" && head.str("/error/data/0/path") == "/entity", "the head is not a player: E_NOT_FOUND");
    CHECK(Reply(lw.s.call("player.info", R"({"entity":"00000000000000ff"})")).code() == "E_NOT_FOUND", "nor is a missing id");
    const Reply byId(lw.s.call("player.info", "{\"entity\":\"" + std::string(kPlayer) + "\"}"));
    CHECK(byId.ok() && byId.json("/result") == Reply(lw.s.call("player.info")).json("/result"), "named by entity, the same");
}

static void sp6_t14_player_teleport_moves_and_stops() {
    LevelWorld lw;
    CHECK(lw.load(kPlayScene).ok(), "the play scene loads");
    CHECK(Reply(lw.s.call("input.inject", R"({"events":[{"axis":"move_forward","value":1}]})")).ok() &&
              Reply(lw.s.call("world.step", R"({"frames":20})")).ok() &&
              Reply(lw.s.call("input.inject", R"({"events":[{"axis":"move_forward","value":null}]})")).ok(),
          "walk 20 steps, then let go");
    const uint64_t before = lw.s.changeCount();
    // A spot on the Step clear of EnemyA (SP7), which stands at (-2.5, 0.3, -2).
    const Reply t(lw.s.call("player.teleport", R"({"pos":[-2,0.3,-1.35],"yawDeg":90})"));
    std::printf("\n      %s", t.json("/result").c_str());
    CHECK(t.ok() && t.json("/result/pos") == "[-2,0.3,-1.35]" && t.str("/result/yawDeg") == "90" &&
              t.json("/result/velocity") == "[0,0,0]",
          "onto the Step, facing -X, at rest; the reply is player.info's");
    CHECK(t.json("/result/ground") == "true" && t.str("/result/on") == "Step" && t.str("/result/surface") == "metal",
          "standing on the Step's metal at once");
    CHECK(lw.s.changeCount() > before, "it changes the world");
    const Reply q(lw.s.call("world.query", R"({"names":["Player"],"fields":["/Transform/position"]})"));
    CHECK(q.json("/result/entities/0/Transform/position") == "[-2,0.3,-1.35]", "its Transform says so");
    CHECK(Reply(lw.s.call("world.step", R"({"frames":10})")).ok(), "10 steps");
    const Reply after(lw.s.call("player.info"));
    CHECK(after.json("/result/pos") == "[-2,0.3,-1.35]" && after.json("/result/velocity") == "[0,0,0]",
          "it stays: the walk's speed did not carry through the teleport");
    const Reply keep(lw.s.call("player.teleport", R"({"pos":[-1,0,1]})"));
    CHECK(keep.ok() && keep.str("/result/yawDeg") == "90" && keep.str("/result/on") == "Floor", "without yawDeg it keeps its facing");

    for (const char* bad : {R"({"pos":[0,0,200000]})", R"({"pos":[-1e6,0,0]})"}) {
        const Reply r(lw.s.call("player.teleport", bad));
        CHECK(r.code() == "E_RANGE" && r.str("/error/data/0/path").rfind("/pos", 0) == 0, "a position past 100 km: E_RANGE at /pos");
    }
    const Reply yaw(lw.s.call("player.teleport", R"({"pos":[0,0,0],"yawDeg":400})"));
    CHECK(yaw.code() == "E_RANGE" && yaw.str("/error/data/0/path") == "/yawDeg", "yawDeg outside [-360, 360]: E_RANGE");
    CHECK(Reply(lw.s.call("player.teleport", R"({"yawDeg":0})")).code() == "E_COMMAND", "pos is required");
    CHECK(Reply(lw.s.call("player.teleport", R"({"pos":[0,0,0],"entity":"5c0b1a7e00000002"})")).code() == "E_NOT_FOUND",
          "the head is not a player");
    LevelWorld empty;
    CHECK(empty.load().ok() && Reply(empty.s.call("player.teleport", R"({"pos":[0,0,0]})")).code() == "E_STATE",
          "no player: E_STATE");
}

static void sp6_t14_physics_contacts_lists_pairs_in_contact() {
    {
        World w;
        api::Session s(w, engineTypes());
        CHECK(Reply(s.call("physics.contacts")).code() == "E_STATE", "a world without the physics system: E_STATE");
    }
    LevelWorld lw;
    CHECK(lw.load().ok(), "the blockout scene loads");
    const Reply none(lw.s.call("physics.contacts"));
    CHECK(none.ok() && none.json("/result") == R"({"contacts":[],"count":0})", "nothing stepped: no contact");
    liftBlockoutCrates(lw);
    CHECK(Reply(lw.s.call("world.step", R"({"frames":120})")).ok(), "the crates fall and settle");
    const Reply r(lw.s.call("physics.contacts"));
    std::printf("\n      %s (%zu bytes)", r.json("/result").c_str(), r.json("/result").size());
    CHECK(r.ok() && r.str("/result/count") == "2" && r.at("/result/contacts") && r.at("/result/contacts")->count == 2,
          "two pairs: CrateA on the Floor, CrateB on CrateA");
    const auto pair = [&](const Reply& x, int i) { return x.str("/result/contacts/" + std::to_string(i) + "/an") + "-" +
                                                          x.str("/result/contacts/" + std::to_string(i) + "/bn"); };
    const std::string p0 = pair(r, 0), p1 = pair(r, 1);
    CHECK((p0 == "CrateA-Floor" || p0 == "Floor-CrateA" || p0 == "CrateA-CrateB" || p0 == "CrateB-CrateA") &&
              (p1 == "CrateA-Floor" || p1 == "Floor-CrateA" || p1 == "CrateA-CrateB" || p1 == "CrateB-CrateA") && p0 != p1,
          "named by their entities");
    CHECK(r.str("/result/contacts/0/a") < r.str("/result/contacts/0/b") && r.str("/result/contacts/0/a") <= r.str("/result/contacts/1/a"),
          "a is the lower id; sorted by a, then b");
    const std::string since = r.str("/result/contacts/0/since");
    CHECK(!since.empty() && std::stoi(since) >= 26 && std::stoi(since) <= 40 && r.at("/result/contacts/0/point") &&
              r.at("/result/contacts/0/normal"),
          "each with the step it began, its point and normal");

    const Reply crateB(lw.s.call("physics.contacts", R"({"names":"CrateB"})"));
    CHECK(crateB.code() == "E_UNKNOWN_FIELD", "an unknown member is refused");
    char bId[17] = {};
    for (auto [e, n] : lw.w.registry().view<const Name>().each())
        if (n.value == "CrateB") std::snprintf(bId, sizeof bId, "%016llx", static_cast<unsigned long long>(lw.w.idOf(e)));
    const Reply onB(lw.s.call("physics.contacts", "{\"entity\":\"" + std::string(bId) + "\"}"));
    CHECK(onB.ok() && onB.str("/result/count") == "1" && (pair(onB, 0) == "CrateA-CrateB" || pair(onB, 0) == "CrateB-CrateA"),
          "entity: only CrateB's pair");
    const Reply one(lw.s.call("physics.contacts", R"({"max":1})"));
    CHECK(one.ok() && one.at("/result/contacts")->count == 1 && one.str("/result/count") == "2" && one.str("/result/more") == "1",
          "max 1: one pair, count 2, one more");
    for (const char* bad : {R"({"max":0})", R"({"max":65})"}) {
        const Reply x(lw.s.call("physics.contacts", bad));
        CHECK(x.code() == "E_RANGE" && x.str("/error/data/0/path") == "/max", "max outside 1..64: E_RANGE");
    }
    const Reply sun(lw.s.call("physics.contacts", R"({"entity":"5c0b10c000000003"})"));
    CHECK(sun.code() == "E_NOT_FOUND" && sun.str("/error/data/0/path") == "/entity", "a light has no body: E_NOT_FOUND");
}

static void sp6_t14_restore_mid_fall_replays_to_the_same_hash() {
    LevelWorld lw;
    CHECK(lw.load().ok(), "the blockout scene loads");
    liftBlockoutCrates(lw);
    CHECK(Reply(lw.s.call("world.step", R"({"frames":15})")).ok() && Reply(lw.s.call("world.snapshot", R"({"name":"mid"})")).ok(),
          "15 steps (the crates in the air), then a snapshot");
    const Reply motion(lw.s.call("world.query", R"({"names":["CrateA"],"fields":["/BodyMotion/linear"]})"));
    std::printf("\n      %s", motion.json("/result").c_str());
    const std::string vy = motion.str("/result/entities/0/BodyMotion/linear/1");
    CHECK(!vy.empty() && std::stod(vy) < -2.0, "world.query sees CrateA's BodyMotion: falling");
    CHECK(Reply(lw.s.call("world.step", R"({"frames":60})")).ok(), "60 steps");
    const std::string first = Reply(lw.s.call("world.hash")).str("/result/hash");
    CHECK(Reply(lw.s.call("world.restore", R"({"name":"mid"})")).ok() && Reply(lw.s.call("world.step", R"({"frames":60})")).ok(),
          "restore, the same 60 steps");
    const std::string second = Reply(lw.s.call("world.hash")).str("/result/hash");
    std::printf("\n      %s / %s", first.c_str(), second.c_str());
    CHECK(!first.empty() && first == second, "the same world.hash");
}

static void sp6_t14_restore_and_replay_a_walk_through_the_api() {
    LevelWorld lw;
    CHECK(lw.load(kPlayScene).ok() && Reply(lw.s.call("world.snapshot", R"({"name":"base"})")).ok(), "the play scene, snapshot");
    const auto walk = [&] {
        return Reply(lw.s.call("input.inject", R"({"events":[{"axis":"move_forward","value":1},{"action":"jump","down":true}]})")).ok() &&
               Reply(lw.s.call("world.step", R"({"frames":1})")).ok() &&
               Reply(lw.s.call("input.inject", R"({"events":[{"action":"jump","down":false}]})")).ok() &&
               Reply(lw.s.call("world.step", R"({"frames":40})")).ok() &&
               Reply(lw.s.call("input.inject", R"({"events":[{"axis":"move_forward","value":null}]})")).ok() &&
               Reply(lw.s.call("world.step", R"({"frames":10})")).ok();
    };
    CHECK(walk(), "walk and jump");
    const std::string first = Reply(lw.s.call("world.hash")).str("/result/hash");
    const std::string pos1 = Reply(lw.s.call("player.info")).json("/result/pos");
    CHECK(Reply(lw.s.call("world.restore", R"({"name":"base"})")).ok() && walk(), "restore, the same walk");
    const std::string second = Reply(lw.s.call("world.hash")).str("/result/hash");
    const std::string pos2 = Reply(lw.s.call("player.info")).json("/result/pos");
    std::printf("\n      %s at %s / %s at %s", first.c_str(), pos1.c_str(), second.c_str(), pos2.c_str());
    CHECK(!first.empty() && first == second && pos2 == pos1, "the same world.hash and the same place");
}

// ---- sub-project 7, step 5: combat.info, combat.events, combat.damage (test 18) --------------------
// docs/specs/2026-10-27-combat-design.md, "The agent-facing surface". The play scene's player is
// 5c0b1a7e00000001, its enemies EnemyA 5c0b1a7e00000010 (body 5c0b1a7e00000011) and EnemyB
// 5c0b1a7e00000020; the Sun 5c0b10c000000003 has no Health.

struct CombatWorld : LevelWorld {
    CombatWorld() { ecs::installCombat(w); }   // as the CLI context and the window host do
};

constexpr const char* kEnemyA = "5c0b1a7e00000010";
constexpr const char* kEnemyB = "5c0b1a7e00000020";

static std::string hashOf(api::Session& s) { return Reply(s.call("world.hash")).str("/result/hash"); }

static void sp7_t18_combat_info() {
    CombatWorld cw;
    const Reply empty(cw.s.call("combat.info"));
    CHECK(empty.ok() && empty.json("/result") == R"({"steps":0,"combatants":[]})", "an empty world: no combatant, no round");
    CHECK(cw.load(kPlayScene).ok(), "the play scene loads");
    const Reply r(cw.s.call("combat.info"));
    std::printf("\n      %s (%zu bytes)", r.json("/result").c_str(), r.json("/result").size());
    CHECK(r.ok() && r.str("/result/steps") == "0" &&
              r.json("/result/round") == R"({"started":false,"phase":"playing","restarts":0})",
          "before any step: the round not started");
    CHECK(r.at("/result/combatants") && r.at("/result/combatants")->count == 3, "three combatants: the player and two enemies");
    CHECK(r.str("/result/combatants/0/entity") == kPlayer && r.str("/result/combatants/0/name") == "Player" &&
              r.json("/result/combatants/0/player") == "true" && r.str("/result/combatants/0/health") == "100" &&
              r.str("/result/combatants/0/max") == "100" && r.json("/result/combatants/0/dead") == "false" &&
              r.json("/result/combatants/0/pos") == "[1.5,0,2.2]",
          "in id order: the player first, at the spawn, 100 of 100");
    CHECK(r.json("/result/combatants/0/weapon") ==
              R"({"ammo":30,"magazine":30,"reloading":0,"bloomDeg":0,"kickDeg":0,"shots":0})",
          "its rifle full before the first step");
    CHECK(!r.at("/result/combatants/0/enemy"), "the player is no enemy");
    CHECK(r.str("/result/combatants/1/entity") == kEnemyA && r.str("/result/combatants/1/name") == "EnemyA" &&
              r.json("/result/combatants/1/pos") == "[-2.5,0.3,-2]" && r.str("/result/combatants/1/weapon/magazine") == "12" &&
              r.str("/result/combatants/1/enemy/state") == "idle" && !r.at("/result/combatants/1/player"),
          "EnemyA at its spawn, idle, a 12-round magazine");
    CHECK(r.str("/result/combatants/2/entity") == kEnemyB && r.str("/result/combatants/2/enemy/state") == "idle", "then EnemyB");
    CHECK(r.json("/result").size() < 1024, "under 1 KB");

    CHECK(Reply(cw.s.call("world.step", R"({"frames":30})")).ok(), "30 steps");
    const Reply a(cw.s.call("combat.info"));
    std::printf("\n      %s", a.json("/result").c_str());
    CHECK(a.str("/result/steps") == "30" && a.json("/result/round/started") == "true" && a.str("/result/round/phase") == "playing",
          "30 combat steps; the round started");
    const std::string sa = a.str("/result/combatants/1/enemy/state"), sb = a.str("/result/combatants/2/enemy/state");
    CHECK((sa == "attack" || sa == "chase") && (sb == "attack" || sb == "chase") && a.at("/result/combatants/1/enemy/lastSeen"),
          "both enemies have seen the player; where, as lastSeen");
    CHECK(Reply(cw.s.call("combat.info", R"({"all":true})")).code() == "E_UNKNOWN_FIELD", "an unknown member is refused");
}

// ---- animation.info (sub-project 10, test 23) ---------------------------------------------------

struct AnimWorld {
    FileAssetResolver files{OHAO_SOURCE_DIR};
    World w;
    api::Session s{w, engineTypes()};
    explicit AnimWorld(bool animation = true) {
        if (animation) ecs::installAnimation(w, files);
        s.setAssetResolver(&files);
    }
    Reply load(const char* scene) {
        const std::string path = (fs::path(OHAO_SOURCE_DIR) / scene).generic_string();
        return Reply(s.call("scene.load", "{\"path\":\"" + path + "\"}"));
    }
};

static void t23_animation_info() {
    AnimWorld none(false);
    CHECK(Reply(none.s.call("animation.info")).code() == "E_STATE", "a world without animation: E_STATE");
    AnimWorld aw;
    CHECK(aw.load("assets/scenes/anim_rig.scene.json").ok(), "the rig scene loads");
    const Reply r(aw.s.call("animation.info"));
    std::printf("\n      %s (%zu bytes)", r.json("/result").c_str(), r.json("/result").size());
    CHECK(r.ok() && r.str("/result/animators/0/entity") == "a17a000000000004" && r.str("/result/animators/0/name") == "Rig" &&
              r.str("/result/animators/0/graph") == "tests/anim/fixtures/rig.animgraph.json",
          "the rig's animator: entity, name, graph");
    CHECK(r.json("/result/animators/0/layers/0") == R"({"state":"wave","clip":"wave","t":0.5})",
          "layer 0: wave at 30 steps, 0.5 s into the clip");
    CHECK(r.json("/result/animators/0/layers/1") == R"({"state":"still","clip":"step","t":0})", "layer 1: still");
    CHECK(r.json("/result/events") == "[]" && r.str("/result/next") == "0", "no step yet: no events");
    CHECK(r.json("/result").size() < 1024, "under 1 KB");
    // Steps: wave's tick at 0.25 s of each 2 s loop; the rig starts at 30 steps (0.5 s), so the next
    // is at 2.25 s, 135 steps: the animation system's step 105, its log seq 0.
    CHECK(Reply(aw.s.call("world.step", R"({"frames":106})")).ok(), "106 steps");
    const Reply e(aw.s.call("animation.info", R"({"since":0})"));
    std::printf("\n      %s", e.json("/result/events").c_str());
    CHECK(e.json("/result/events/0") == R"({"seq":1,"step":105,"entity":"a17a000000000004","layer":0,"clip":"wave","name":"tick"})" &&
              e.str("/result/next") == "1",
          "the tick at step 105, numbered from 1; next 1");
    CHECK(Reply(aw.s.call("animation.info", R"({"since":1})")).json("/result/events") == "[]", "read on from next: nothing new");
    // A forced state: component.set on Animator, as any edit.
    CHECK(Reply(aw.s.call("world.apply",
                          R"({"commands":[{"op":"component.set","id":"a17a000000000004","type":"Animator","value":)"
                          R"({"graph":{"path":"tests/anim/fixtures/rig.animgraph.json"},"layers":[{"state":"held","steps":4},)"
                          R"({"state":"still","steps":0}]}}]})"))
              .ok(),
          "component.set forces a state");
    const Reply f(aw.s.call("animation.info", R"({"entity":"a17a000000000004","max":0})"));
    CHECK(f.json("/result/animators/0/layers/0") == R"({"state":"held","clip":"raise","t":1})", "held: raise at 1 s");
    CHECK(Reply(aw.s.call("animation.info", R"({"entity":"a17a000000000001"})")).code() == "E_NOT_FOUND",
          "an entity without an Animator: E_NOT_FOUND");
}

static void sp7_t18_combat_events_cursor() {
    CombatWorld cw;
    CHECK(cw.load(kPlayScene).ok(), "the play scene loads");
    const Reply none(cw.s.call("combat.events"));
    CHECK(none.ok() && none.json("/result") == R"({"events":[],"next":0})", "no step yet: {events: [], next: 0}");
    // The rifle at 10 Hz, held 13 steps: rounds on steps 1, 7 and 13.
    CHECK(Reply(cw.s.call("input.inject", R"({"events":[{"action":"fire","down":true}]})")).ok() &&
              Reply(cw.s.call("world.step", R"({"frames":13})")).ok() &&
              Reply(cw.s.call("input.inject", R"({"events":[{"action":"fire","down":false}]})")).ok() &&
              Reply(cw.s.call("world.step", R"({"frames":1})")).ok(),
          "fire held 13 steps");
    const Reply all(cw.s.call("combat.events", R"({"max":64})"));
    std::printf("\n      %.700s", all.text.c_str());
    CHECK(all.ok() && all.at("/result/events") && all.at("/result/events")->count >= 4, "the burst's events");
    int shots = 0, starts = 0;
    std::size_t largest = 0;
    bool shotShape = true, seqs = true;
    for (uint32_t i = 0; i < all.at("/result/events")->count; ++i) {
        const std::string at = "/result/events/" + std::to_string(i);
        largest = std::max(largest, all.json(at).size());
        seqs &= all.str(at + "/seq") == std::to_string(i + 1);
        const std::string kind = all.str(at + "/kind");
        starts += kind == "round_start";
        if (kind != "shot" || all.str(at + "/source") != kPlayer) continue;
        ++shots;
        shotShape &= all.str(at + "/sn") == "Player" && all.at(at + "/point") && all.at(at + "/dir") && all.at(at + "/coneDeg");
    }
    CHECK(shots == 3 && shotShape, "three rounds by the player, each with its eye, direction and cone");
    CHECK(starts == 1 && seqs, "one round_start; seqs 1, 2, 3, ... (as physics.events)");
    CHECK(largest < 220, "every event under 220 bytes");
    const std::string next = all.str("/result/next");
    CHECK(next == std::to_string(all.at("/result/events")->count), "next is the last event's seq");
    const Reply after(cw.s.call("combat.events", "{\"since\":" + next + "}"));
    CHECK(after.ok() && after.at("/result/events")->count == 0 && after.str("/result/next") == next && !after.at("/result/dropped"),
          "nothing after next; next stays");
    const Reply one(cw.s.call("combat.events", R"({"since":1,"max":1})"));
    CHECK(one.ok() && one.at("/result/events")->count == 1 && one.str("/result/events/0/seq") == "2" && one.str("/result/next") == "2",
          "since 1, max 1: event 2, next 2");
    const Reply ahead(cw.s.call("combat.events", R"({"since":100000})"));
    CHECK(ahead.ok() && ahead.str("/result/next") == next, "a cursor past the log comes back as the log's end");
}

static void sp7_t18_combat_damage_kills() {
    CombatWorld cw;
    CHECK(cw.load(kPlayScene).ok() && Reply(cw.s.call("world.step", R"({"frames":1})")).ok(), "the play scene, one step");
    const std::string start = hashOf(cw.s);   // the round's start: the world after its first step
    CHECK(Reply(cw.s.call("world.snapshot", R"({"name":"start"})")).ok(), "a snapshot of it");
    const uint64_t before = cw.s.changeCount();
    const Reply d(cw.s.call("combat.damage", "{\"entity\":\"" + std::string(kEnemyA) + "\",\"amount\":30,\"source\":\"" +
                                                 kPlayer + "\"}"));
    std::printf("\n      %s", d.json("/result").c_str());
    CHECK(d.ok() && d.json("/result") == R"({"entity":"5c0b1a7e00000010","name":"EnemyA","health":70,"max":100,"dead":false,"taken":30})",
          "30 off EnemyA: 70 of 100");
    CHECK(cw.s.changeCount() > before, "it changes the world");
    const Reply body(cw.s.call("combat.damage", R"({"entity":"5c0b1a7e00000011","amount":80})"));
    CHECK(body.ok() && body.str("/result/entity") == kEnemyA && body.str("/result/health") == "0" &&
              body.json("/result/dead") == "true" && body.str("/result/taken") == "70",
          "damage to its body goes to EnemyA's Health (an ancestor's): 70 taken, dead");
    const Reply again(cw.s.call("combat.damage", "{\"entity\":\"" + std::string(kEnemyA) + "\",\"amount\":10}"));
    CHECK(again.ok() && again.str("/result/taken") == "0" && again.str("/result/health") == "0", "the dead take none");
    const Reply ev(cw.s.call("combat.events", R"({"max":64})"));
    int damages = 0, deaths = 0;
    bool sourced = false;
    for (uint32_t i = 0; ev.at("/result/events") && i < ev.at("/result/events")->count; ++i) {
        const std::string at = "/result/events/" + std::to_string(i);
        if (ev.str(at + "/target") != kEnemyA) continue;
        if (ev.str(at + "/kind") == "damage") {
            ++damages;
            sourced |= ev.str(at + "/source") == kPlayer && ev.str(at + "/amount") == "30" && ev.str(at + "/health") == "70";
        }
        deaths += ev.str(at + "/kind") == "death";
    }
    CHECK(damages == 2 && deaths == 1 && sourced, "two damage events (the first from the player) and one death in the log");
    CHECK(Reply(cw.s.call("world.step", R"({"frames":1})")).ok(), "a step");
    const Reply i1(cw.s.call("combat.info"));
    CHECK(i1.str("/result/combatants/1/enemy/state") == "dead" && i1.json("/result/combatants/1/dead") == "true",
          "the next step lays EnemyA down: state dead");

    CHECK(Reply(cw.s.call("combat.damage", "{\"entity\":\"" + std::string(kEnemyB) + "\",\"amount\":100}")).ok() &&
              Reply(cw.s.call("world.step", R"({"frames":1})")).ok(),
          "EnemyB killed, a step");
    const Reply won(cw.s.call("combat.info"));
    CHECK(won.str("/result/round/phase") == "won", "both enemies dead: the round is won");
    const Reply wins(cw.s.call("combat.events", R"({"since":0,"max":64})"));
    CHECK(wins.text.find("\"kind\":\"win\"") != std::string::npos, "a win event");

    const Reply killed(cw.s.call("combat.damage", "{\"entity\":\"" + std::string(kPlayer) + "\",\"amount\":1000}"));
    CHECK(killed.ok() && killed.json("/result/dead") == "true" && killed.str("/result/taken") == "100", "the player killed");
    CHECK(Reply(cw.s.call("world.step", R"({"frames":1})")).ok(), "a step");
    const Reply dead(cw.s.call("combat.info"));
    std::printf("\n      %s", dead.json("/result/round").c_str());
    CHECK(dead.str("/result/round/phase") == "dead" && dead.at("/result/round/restartIn") &&
              std::fabs(std::stod(dead.str("/result/round/restartIn")) - 2.98) < 0.011,
          "the round: dead, restarting in 2.98 s");
    CHECK(Reply(cw.s.call("world.step", R"({"frames":179})")).ok(), "179 more steps: 3 s dead");
    const Reply back(cw.s.call("combat.info"));
    std::printf("\n      %s", back.json("/result").c_str());
    CHECK(back.str("/result/round/phase") == "playing" && back.str("/result/round/restarts") == "1",
          "the round restarted once");
    CHECK(back.str("/result/combatants/0/health") == "100" && back.str("/result/combatants/1/health") == "100" &&
              back.str("/result/combatants/2/health") == "100" && back.str("/result/combatants/1/enemy/state") != "dead",
          "everyone back at 100, EnemyA up again");
    const Reply diff(cw.s.call("world.diff", R"({"snapshot":"start"})"));
    if (hashOf(cw.s) != start) std::printf("\n      diff from the start: %.1500s", diff.json("/result").c_str());
    CHECK(hashOf(cw.s) == start, "world.hash is the round start's again");
}

static void sp7_t18_combat_params_refused() {
    {
        World w;
        api::Session s(w, engineTypes());
        CHECK(Reply(s.call("combat.info")).code() == "E_STATE" && Reply(s.call("combat.events")).code() == "E_STATE" &&
                  Reply(s.call("combat.damage", R"({"entity":"5c0b1a7e00000010","amount":1})")).code() == "E_STATE",
              "a world without the combat system: E_STATE for all three");
    }
    CombatWorld cw;
    CHECK(cw.load(kPlayScene).ok(), "the play scene loads");
    for (const char* amount : {"-1", "10000.5", "1e9"}) {
        const Reply r(cw.s.call("combat.damage", std::string(R"({"entity":"5c0b1a7e00000020","amount":)") + amount + "}"));
        CHECK(r.code() == "E_RANGE" && r.str("/error/data/0/path") == "/amount", "amount outside [0, 10000]: E_RANGE at /amount");
    }
    CHECK(Reply(cw.s.call("combat.damage", R"({"entity":"5c0b1a7e00000020","amount":10000})")).ok(), "10000 is allowed");
    CHECK(Reply(cw.s.call("combat.damage", R"({"amount":5})")).code() == "E_COMMAND", "entity is required");
    CHECK(Reply(cw.s.call("combat.damage", R"({"entity":"5c0b1a7e00000010"})")).code() == "E_COMMAND", "amount is required");
    const Reply missing(cw.s.call("combat.damage", R"({"entity":"00000000000000ff","amount":5})"));
    CHECK(missing.code() == "E_NOT_FOUND" && missing.str("/error/data/0/path") == "/entity", "an id no entity has: E_NOT_FOUND");
    const Reply sun(cw.s.call("combat.damage", R"({"entity":"5c0b10c000000003","amount":5})"));
    CHECK(sun.code() == "E_NOT_FOUND" && sun.str("/error/data/0/path") == "/entity", "the Sun has no Health: E_NOT_FOUND");
    const Reply src(cw.s.call("combat.damage", R"({"entity":"5c0b1a7e00000010","amount":5,"source":"00000000000000ff"})"));
    CHECK(src.code() == "E_NOT_FOUND" && src.str("/error/data/0/path") == "/source", "a source no entity has: E_NOT_FOUND at /source");
    CHECK(Reply(cw.s.call("combat.damage", R"({"entity":"5c0b1a7e00000010","amount":5,"hp":1})")).code() == "E_UNKNOWN_FIELD",
          "an unknown member is refused");
    const Reply untouched(cw.s.call("combat.info"));
    CHECK(untouched.str("/result/combatants/1/health") == "100", "no refused call took any health");
    for (const char* bad : {R"({"max":0})", R"({"max":65})", R"({"since":-1})"}) {
        const Reply r(cw.s.call("combat.events", bad));
        CHECK(r.code() == "E_RANGE", "combat.events: max outside 1..64, since below 0: E_RANGE");
    }
}

// ---- audio.info, audio.mix (sub-project 11, test 29) ------------------------------------------------
// docs/specs/2026-10-28-audio-design.md, "The agent-facing surface". The play scene's bank is
// assets/audio/slice.audio.json (ten buses, snapshots default, dead, won).

struct AudioWorld : LevelWorld {
    AudioWorld() {   // as the CLI context and the window host install them
        ecs::installCombat(w);
        ecs::installAnimation(w, files);
        ecs::installAudio(w, files);
    }
    // Faces EnemyA (at (-2.5, 0.3, -2) from the spawn (1.5, 0, 2.2): yaw 43.6) and holds fire 20 steps
    // out of every 40, for `steps` steps.
    void fight(int steps) {
        (void)s.call("player.teleport", R"({"pos":[1.5,0,2.2],"yawDeg":43.6})");
        for (int i = 0; i < steps; i += 20) {
            const bool down = (i / 20) % 2 == 0;
            (void)s.call("input.inject", std::string(R"({"events":[{"action":"fire","down":)") + (down ? "true" : "false") + "}]}");
            (void)s.call("world.step", R"({"frames":20})");
        }
    }
};

// A window host's audio half: what it reports, and the overrides audio.mix hands it.
struct FakeAudioHost : api::Host {
    api::AudioMixOverrides mix;
    int mixes{0};
    std::vector<float> busDb;          // what the device drives each bank bus to
    std::vector<uint32_t> busVoices;   // what sounds on each
    [[nodiscard]] api::HostKind kind() const noexcept override { return api::HostKind::Window; }
    [[nodiscard]] api::RenderOutcome capture(const api::RenderRequest&) override { return {}; }
    [[nodiscard]] std::optional<api::RenderStats> renderStats(int) override {
        api::RenderStats st;
        st.frames = 1;
        st.audio = api::RenderStats::Audio{0.0712, 7, 2};
        return st;
    }
    [[nodiscard]] std::optional<api::AudioDeviceState> audioDevice() override {
        api::AudioDeviceState d;
        d.sampleRate = 48000;
        d.horizonMs = 1000.0 / 60.0;
        d.voices = 3;
        d.late = 2;
        d.mixMs = 0.0712;
        d.busDb = busDb;
        d.busVoices = busVoices;
        return d;
    }
    void setAudioMix(const api::AudioMixOverrides& m) override {
        mix = m;
        ++mixes;
    }
};

static std::string busEntry(const Reply& r, const std::string& name) {
    for (uint32_t i = 0; r.at("/result/buses") && i < r.at("/result/buses")->count; ++i)
        if (r.str("/result/buses/" + std::to_string(i) + "/name") == name) return r.json("/result/buses/" + std::to_string(i));
    return {};
}

static void t29_audio_info() {
    {
        World w;
        api::Session s(w, engineTypes());
        CHECK(Reply(s.call("audio.info")).code() == "E_STATE" && Reply(s.call("audio.mix", R"({"clear":true})")).code() == "E_STATE",
              "a world without the voice schedule: E_STATE for both");
    }
    AudioWorld aw;
    CHECK(aw.load(kPlayScene).ok(), "the play scene loads");
    CHECK(Reply(aw.s.call("world.step", R"({"frames":1})")).ok(), "one step: the schedule reads the scene's bank");
    const Reply first(aw.s.call("audio.info"));
    std::printf("\n      %.700s", first.json("/result").c_str());
    CHECK(first.ok() && first.str("/result/device") == "none" && !first.at("/result/sampleRate") && !first.at("/result/late"),
          "headless: device none, no device half");
    CHECK(first.str("/result/horizonMs") == "16.67", "the horizon: one step");
    CHECK(first.json("/result/snapshot") == R"({"name":"default","blend":0})" && first.str("/result/zone") == "room",
          "the default snapshot; the listener in the room's zone");
    CHECK(first.json("/result/listener/pos").size() > 2, "the listener at the step");
    CHECK(first.at("/result/buses") && first.at("/result/buses")->count == 10 && first.str("/result/buses/0/name") == "master" &&
              busEntry(first, "weapons") == R"({"name":"weapons","cap":12})",
          "the bank's ten buses in its order, with their caps (a level of 0 dB left out)");
    CHECK(first.json("/result/voices") == R"({"playing":0,"dropped":0,"stolen":0})" && first.at("/result/rays"),
          "no voice held yet (a bed is the zone's, not a voice); nothing dropped or stolen");
    CHECK(first.str("/result/commands/0/seq") == "1" && first.str("/result/commands/0/op") == "zone" &&
              first.str("/result/commands/0/zone") == "room" && first.str("/result/commands/0/bed") == "bed_room",
          "the first command: the room's zone and its bed");

    aw.fight(120);
    const Reply mid(aw.s.call("audio.info", R"({"since":0,"max":64})"));
    bool fire = false, enemy = false;
    for (uint32_t i = 0; mid.at("/result/commands") && i < mid.at("/result/commands")->count; ++i) {
        const std::string at = "/result/commands/" + std::to_string(i);
        fire |= mid.str(at + "/op") == "play" && mid.str(at + "/sound") == "fire" && !mid.at(at + "/pos");
        enemy |= mid.str(at + "/op") == "play" && mid.str(at + "/sound") == "enemy_fire" && mid.at(at + "/pos");
    }
    CHECK(fire && enemy, "the player's rounds at the ear, the enemies' at their place");
    CHECK(std::stoull(mid.str("/result/rays/cast")) > 0, "the enemies' rounds cast occlusion rays");
    const std::string next = mid.str("/result/next");
    const Reply none(aw.s.call("audio.info", "{\"since\":" + next + "}"));
    CHECK(none.ok() && none.at("/result/commands")->count == 0 && none.str("/result/next") == next, "nothing after next; next stays");
    const Reply one(aw.s.call("audio.info", R"({"since":1,"max":1})"));
    CHECK(one.at("/result/commands")->count == 1 && one.str("/result/commands/0/seq") == "2" && one.str("/result/next") == "2",
          "since 1, max 1: command 2, next 2");
    const Reply zero(aw.s.call("audio.info", R"({"max":0})"));
    CHECK(zero.ok() && zero.at("/result/commands")->count == 0, "max 0: no commands");

    // The budget: mid-fight, 16 commands, at most 2 KB (serve_bench audio_info_bytes).
    const Reply sixteen(aw.s.call("audio.info", "{\"since\":" + std::to_string(std::max(0, std::stoi(next) - 16)) + "}"));
    std::printf("\n      max 16 mid-fight: %zu bytes", sixteen.json("/result").size());
    CHECK(sixteen.at("/result/commands")->count == 16 && sixteen.json("/result").size() <= 2048, "16 commands in at most 2 KB");

    for (const char* bad : {R"({"max":65})", R"({"since":-1})"})
        CHECK(Reply(aw.s.call("audio.info", bad)).code() == "E_RANGE", "max over 64, since below 0: E_RANGE");
    CHECK(Reply(aw.s.call("audio.info", R"({"bus":"weapons"})")).code() == "E_UNKNOWN_FIELD", "an unknown member is refused");

    FakeAudioHost host;
    aw.s.setHost(&host);
    const Reply win(aw.s.call("audio.info", R"({"max":0})"));
    std::printf("\n      window: device %s, rate %s, late %s, mixMs %s, voices %s", win.str("/result/device").c_str(),
                win.str("/result/sampleRate").c_str(), win.str("/result/late").c_str(), win.str("/result/mixMs").c_str(),
                win.json("/result/voices").c_str());
    CHECK(win.str("/result/device") == "default" && win.str("/result/sampleRate") == "48000" && win.str("/result/late") == "2" &&
              win.str("/result/mixMs") == "0.071" && win.str("/result/voices/device") == "3",
          "a window adds the device's half: rate, late commands, the mix's cost, its voices");
    host.busDb.assign(10, 0.0f);
    host.busVoices.assign(10, 0);
    host.busDb[2] = -96.0f;   // weapons, as a mute drives it
    host.busVoices[2] = 2;
    const Reply buses(aw.s.call("audio.info", R"({"max":0})"));
    std::printf("\n      window buses: %s | %s", busEntry(buses, "master").c_str(), busEntry(buses, "weapons").c_str());
    CHECK(busEntry(buses, "master") == R"({"name":"master"})" &&
              busEntry(buses, "weapons").find(R"("out":-96,"playing":2)") != std::string::npos,
          "per bus, the device's level when it is not the game's, and the voices sounding on it");
    const Reply stats(aw.s.call("render.stats"));
    CHECK(stats.json("/result/audio") == R"({"mixMs":0.071,"voices":7,"late":2})", "render.stats carries the host's audio");
    aw.s.setHost(nullptr);
}

static void t29_audio_mix() {
    AudioWorld aw;
    CHECK(aw.load(kPlayScene).ok() && Reply(aw.s.call("world.step", R"({"frames":1})")).ok(), "the play scene, one step");
    FakeAudioHost host;
    aw.s.setHost(&host);
    const std::string hash0 = hashOf(aw.s);
    const uint64_t changes0 = aw.s.changeCount();
    CHECK(Reply(aw.s.call("world.snapshot", R"({"name":"before"})")).ok(), "a snapshot");

    const Reply mute(aw.s.call("audio.mix", R"({"bus":"weapons","mute":true})"));
    std::printf("\n      %.400s", mute.json("/result").c_str());
    CHECK(mute.ok() && busEntry(mute, "weapons") == R"({"name":"weapons","cap":12,"mix":{"mute":true}})" &&
              !mute.at("/result/commands"),
          "weapons muted; the reply is audio.info's without commands");
    CHECK(Reply(aw.s.call("audio.mix", R"({"bus":"music","db":-12.5})")).ok() &&
              Reply(aw.s.call("audio.mix", R"({"bus":"foley","solo":true})")).ok() &&
              Reply(aw.s.call("audio.mix", R"({"snapshot":"dead"})")).ok(),
          "music at -12.5 dB, foley soloed, the dead snapshot forced");
    const Reply info(aw.s.call("audio.info", R"({"max":0})"));
    CHECK(busEntry(info, "music") == R"({"name":"music","cap":2,"mix":{"db":-12.5}})" &&
              busEntry(info, "foley").find(R"("mix":{"solo":true})") != std::string::npos &&
              info.json("/result/snapshot") == R"({"name":"default","blend":0,"forced":"dead"})",
          "audio.info reports every override beside the game's mix (the schedule's snapshot stays default)");
    CHECK(host.mixes == 4 && host.mix.buses.size() == 3 && host.mix.snapshot == "dead" && host.mix.buses[0].name == "weapons" &&
              host.mix.buses[0].mute && host.mix.buses[1].db && *host.mix.buses[1].db == -12.5f && host.mix.buses[2].solo,
          "the host got all of them, each time");

    CHECK(hashOf(aw.s) == hash0 && aw.s.changeCount() == changes0, "world.hash unchanged; no change counted");
    CHECK(Reply(aw.s.call("world.restore", R"({"name":"before"})")).ok(), "a restore");
    const Reply restored(aw.s.call("audio.info", R"({"max":0})"));
    CHECK(busEntry(restored, "weapons").find(R"("mix":{"mute":true})") != std::string::npos &&
              restored.str("/result/snapshot/forced") == "dead",
          "the overrides are the session's: a restore keeps them (no snapshot holds them)");

    CHECK(Reply(aw.s.call("audio.mix", R"({"bus":"weapons","mute":false})")).ok() &&
              busEntry(Reply(aw.s.call("audio.info", R"({"max":0})")), "weapons") == R"({"name":"weapons","cap":12})",
          "unmuted with nothing else set: the bus has no override left");
    CHECK(Reply(aw.s.call("audio.mix", R"({"snapshot":""})")).ok() &&
              !Reply(aw.s.call("audio.info", R"({"max":0})")).at("/result/snapshot/forced"),
          "snapshot \"\": the schedule's again");

    const Reply noBus(aw.s.call("audio.mix", R"({"db":-6})"));
    CHECK(noBus.code() == "E_COMMAND", "db without a bus: E_COMMAND");
    CHECK(Reply(aw.s.call("audio.mix", R"({"bus":"weapons"})")).code() == "E_COMMAND", "a bus alone: E_COMMAND");
    const Reply unknown(aw.s.call("audio.mix", R"({"bus":"lasers","mute":true})"));
    CHECK(unknown.code() == "E_NOT_FOUND" && unknown.str("/error/data/0/path") == "/bus", "a bus the bank lacks: E_NOT_FOUND at /bus");
    const Reply snap(aw.s.call("audio.mix", R"({"snapshot":"underwater"})"));
    CHECK(snap.code() == "E_NOT_FOUND" && snap.str("/error/data/0/path") == "/snapshot", "a snapshot the bank lacks: E_NOT_FOUND");
    const Reply loud(aw.s.call("audio.mix", R"({"bus":"music","db":20})"));
    CHECK(loud.code() == "E_RANGE" && loud.str("/error/data/0/path") == "/db", "db over +12: E_RANGE at /db");
    CHECK(host.mixes == 6, "a refused call hands the host nothing");

    const Reply cleared(aw.s.call("audio.mix", R"({"clear":true})"));
    CHECK(cleared.ok() && cleared.json("/result").find("\"mix\"") == std::string::npos && host.mix.buses.empty() &&
              host.mix.snapshot.empty(),
          "clear: no override left, here or in the host");
    aw.s.setHost(nullptr);

    // The schedule never sees an override: the same fight with and without them writes the same commands.
    AudioWorld plain, mixed;
    CHECK(plain.load(kPlayScene).ok() && mixed.load(kPlayScene).ok(), "two play scenes");
    CHECK(Reply(mixed.s.call("world.step", R"({"frames":1})")).ok() && Reply(plain.s.call("world.step", R"({"frames":1})")).ok() &&
              Reply(mixed.s.call("audio.mix", R"({"bus":"weapons","mute":true})")).ok() &&
              Reply(mixed.s.call("audio.mix", R"({"snapshot":"won"})")).ok(),
          "one of them muted and forced to won");
    plain.fight(120);
    mixed.fight(120);
    const Reply a(plain.s.call("audio.info", R"({"max":64})")), b(mixed.s.call("audio.info", R"({"max":64})"));
    const std::string ca = a.json("/result/commands"), cb = b.json("/result/commands");
    if (ca != cb) {
        std::size_t at = 0;
        while (at < ca.size() && at < cb.size() && ca[at] == cb[at]) ++at;
        const std::size_t from = at > 160 ? at - 160 : 0;
        std::printf("\n      differ at %zu:\n      %s\n      %s", at, ca.substr(from, 320).c_str(), cb.substr(from, 320).c_str());
    }
    CHECK(ca == cb, "the same commands");
}

static void sp5_t08_level_reload_keeps_world_hash() {
    LevelWorld lw;
    CHECK(lw.load().ok(), "the blockout scene loads");
    const std::string before = Reply(lw.s.call("world.hash")).str("/result/hash");
    const Reply r(lw.s.call("level.reload"));
    CHECK(r.ok() && r.str("/result/nodes") == "18" && r.json("/result/bodies") == R"({"static":12,"dynamic":2})",
          "level.reload answers as level.info, after re-reading the file");
    CHECK(Reply(lw.s.call("world.hash")).str("/result/hash") == before, "an unchanged file keeps world.hash");
    const Reply hit(lw.s.call("physics.raycast", R"({"from":[1.5,0.5,2.2],"dir":[0,-1,0],"max":2})"));
    CHECK(hit.ok() && hit.str("/result/name") == "Floor", "and the rebuilt bodies collide");
    CHECK(Reply(lw.s.call("level.reload", R"({"entity":"5c0b10c000000003"})")).code() == "E_NOT_FOUND",
          "a light is not a level: E_NOT_FOUND");
}

static void sp5_t08_warnings_reach_level_and_physics_info() {
    LevelWorld lw;
    const Reply loaded(lw.s.call("scene.load", R"({"scene":{"format":"ohao.scene","version":1,"entities":[
        {"id":"00000000000000a1","Name":{"value":"Fixture"},
         "Level":{"asset":{"path":"tests/level/fixtures/level_physics.gltf"}}}]}})"));
    CHECK(loaded.ok(), "a level without spawns, with a dynamic mesh collider, loads");
    const Reply info(lw.s.call("level.info"));
    std::printf("\n      %s", info.json("/result/warnings").c_str());
    const std::string warnings = info.json("/result/warnings");
    CHECK(info.ok() && warnings.find("W_LEVEL_NO_PLAYER_SPAWN") != std::string::npos &&
              warnings.find("W_LEVEL_DYNAMIC_MESH") != std::string::npos,
          "level.info warns of the missing player spawn and the demoted mesh");
    const Reply phys(lw.s.call("physics.info"));
    CHECK(phys.ok() && phys.json("/result/warnings").find("W_LEVEL_DYNAMIC_MESH") != std::string::npos &&
              phys.json("/result/warnings").find("DynMesh") != std::string::npos,
          "physics.info lists the warning with its entity's name");
}

int main() {
    std::printf("api_tests\n");
    RUN(t09_method_table_dispatch_and_errors);
    RUN(t09_schema_get_includes_methods);
    RUN(t09_render_capture_needs_a_host_and_checks_its_params);
    RUN(sp2b_t14_render_stats_reports_the_hosts_medians);
    RUN(sp2b_t14_render_settings_reads_changes_and_defaults_captures);
    RUN(sp4_t16_an_inline_capture_carries_its_image_beside_the_result);
    RUN(sp4_t16_change_count_rises_only_for_methods_that_change_the_world);
    RUN(t09_each_command_applies_and_returns_its_inverse);
    RUN(t09_duplicate_copies_a_subtree_with_fresh_ids);
    RUN(t09_failing_command_rolls_back_the_batch);
    RUN(t09_batch_then_inverse_is_byte_identical);
    RUN(t09_command_built_world_saves_like_the_file);
    RUN(t09_dollar_refs_name_entities_created_in_the_batch);
    RUN(t09_command_errors_are_precise_and_terse);
    RUN(t09_query_filters_and_projects);
    RUN(t09_summary_is_small_and_complete);
    RUN(t09_scene_methods_load_save_validate);
    RUN(t10_snapshot_and_restore_through_the_api);
    RUN(t11_validate_checks_assets_when_asked);
    RUN(t20_commands_keep_one_camera_and_environment);
    RUN(sp4_t08_params_structs_lint_clean_and_close_their_schemas);
    RUN(sp4_engine_info_and_log_read);
    RUN(sp4_scene_load_replace_clears_and_keeps_snapshots);
    RUN(sp4_t15_paths_are_confined_to_the_sandbox);
    RUN(sp4_t15_batch_and_inject_caps_hold);
    RUN(sp4_t07_cancel_leaves_whole_steps);
    RUN(sp4_t11_simulate_runs_once_per_step_and_sync_once_per_batch);
    RUN(sp4_t11_world_step_watch_returns_fields_after_the_last_step);
    RUN(sp4_t11_time_rules_per_host_and_mode);
    RUN(sp4_t11_dt_defaults_to_the_fixed_step);
    RUN(sp4_t11_realtime_paused_stepped_reaches_the_lockstep_hash);
    RUN(sp4_t12_hash_is_stable_across_save_and_load);
    RUN(sp4_t12_hash_changes_on_any_authoring_change);
    RUN(sp4_t12_diff_equals_a_diff_of_the_two_saves);
    RUN(sp4_t12_diff_respects_limit_fields_and_names);
    RUN(sp4_t13_injection_is_stamped_by_step);
    RUN(sp4_t13_axis_override_holds_and_releases_to_the_keys);
    RUN(sp4_t13_input_actions_rebind_on_change);
    RUN(sp4_t13_focus_lost_releases_everything);
    RUN(sp4_t13_unknown_key_names_are_e_enum_with_a_suggestion);
    RUN(sp4_t13_record_then_replay_matches_every_checkpoint);
    RUN(sp4_t13_a_tampered_recording_fails_at_the_right_step);
    RUN(sp4_t13_a_realtime_recording_with_dropped_time_replays_exactly);
    RUN(sp5_t08_level_info_counts_and_spawns);
    RUN(sp5_t08_level_info_is_small);
    RUN(sp5_t08_raycast_hits_floor);
    RUN(sp5_t08_raycast_params_refused);
    RUN(sp5_t08_physics_info_counts);
    RUN(sp5_t08_level_reload_keeps_world_hash);
    RUN(sp5_t08_warnings_reach_level_and_physics_info);
    RUN(sp6_t11_physics_events_after_drop);
    RUN(sp6_t11_events_cursor_and_dropped);
    RUN(sp6_t11_events_params_refused);
    RUN(sp6_t11_physics_info_counts_steps);
    RUN(sp6_t13_footsteps_in_physics_events);
    RUN(sp6_t14_player_info_after_load_and_walk);
    RUN(sp6_t14_player_teleport_moves_and_stops);
    RUN(sp6_t14_physics_contacts_lists_pairs_in_contact);
    RUN(sp6_t14_restore_mid_fall_replays_to_the_same_hash);
    RUN(sp6_t14_restore_and_replay_a_walk_through_the_api);
    RUN(sp7_t18_combat_info);
    RUN(t23_animation_info);
    RUN(sp7_t18_combat_events_cursor);
    RUN(sp7_t18_combat_damage_kills);
    RUN(sp7_t18_combat_params_refused);
    RUN(t29_audio_info);
    RUN(t29_audio_mix);
    RUN(bench_apply_and_query_10k);

    std::printf("Results: %d/%d passed\n", g_pass, g_pass + g_fail);
    JsonWriter s;
    s.beginObject();
    s.key("suite"); s.string("api_tests");
    s.key("passed"); s.int64(g_pass);
    s.key("failed"); s.int64(g_fail);
    s.key("failures"); s.beginArray();
    for (const auto& f : g_failures) s.string(f);
    s.endArray();
    s.endObject();
    std::printf("%s\n", s.str().c_str());
    return g_fail == 0 ? 0 : 1;
}
