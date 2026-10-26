#include "level/level_hints.hpp"

#include "ecs/json_parse.hpp"
#include "ecs/scene_types.hpp"
#include "reflect/json_value.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/registry.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <new>
#include <span>
#include <string_view>
#include <tuple>

namespace ohao::level {

using reflect::Diagnostic;
using reflect::JsonDocument;
using reflect::JsonNode;
using reflect::JsonType;

namespace {

constexpr std::array<std::string_view, 8> kHintKeys = {"col", "body", "mass", "fric", "rest", "vis", "spawn", "socket"};
constexpr std::array<std::string_view, 6> kShapes = {"box", "sphere", "capsule", "convex", "mesh", "none"};   // ColliderShape order
constexpr std::array<std::string_view, 3> kMotions = {"static", "dynamic", "kinematic"};                       // Motion order
constexpr std::size_t kMaxSpawn = 32;    // SpawnPoint.tag's limit
constexpr std::size_t kMaxSocket = 64;   // Socket.name's limit

std::size_t codePoints(std::string_view s) noexcept {
    std::size_t n = 0;
    for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0u) != 0x80u ? 1 : 0;
    return n;
}

std::string listOf(std::span<const std::string_view> names) {
    std::string out;
    for (const auto n : names) out += (out.empty() ? "" : ", ") + std::string(n);
    return out;
}

// A value as compact JSON, cut short, for messages.
std::string shown(const JsonDocument& d, const JsonNode& n) {
    reflect::JsonWriter w;
    reflect::writeJson(w, d, n);
    std::string s = w.take();
    if (s.size() > 48) s = s.substr(0, 45) + "...";
    return s;
}

// Where one node's (or the default scene's) diagnostics go, named by its path and the key.
struct Sink {
    std::string path;   // the node's path; "" for the default scene
    std::vector<Diagnostic>& out;

    [[nodiscard]] std::string where(std::string_view key) const {
        return (path.empty() ? std::string("the default scene's extras") : "node '" + path + "' extras") + " '" +
               std::string(key) + "'";
    }
    void add(std::string_view code, std::string_view key, const std::string& what, std::string suggestion = {}) {
        Diagnostic d;
        d.code = code;
        d.path = path;
        d.message = where(key) + ": " + what;
        d.suggestion = std::move(suggestion);
        out.push_back(std::move(d));
    }
    // A diagnostic from the scene reader (a passed-through component): its code kept.
    void add(Diagnostic d, std::string_view key) {
        d.message = where(key) + ": " + d.message;
        d.path = path;
        d.offset = d.line = d.column = 0;
        out.push_back(std::move(d));
    }
};

std::optional<double> number(const JsonNode& n) {
    if (n.type != JsonType::Number) return std::nullopt;
    double v = 0.0;
    const auto r = std::from_chars(n.text.data(), n.text.data() + n.text.size(), v);
    if (r.ec != std::errc{} || !std::isfinite(v)) return std::nullopt;
    return v;
}

template <class E, std::size_t N>
std::optional<E> named(const JsonNode& n, const std::array<std::string_view, N>& names) {
    if (n.type != JsonType::String) return std::nullopt;
    for (std::size_t i = 0; i < N; ++i)
        if (n.text == names[i]) return static_cast<E>(i);
    return std::nullopt;
}

// col and body: the keys a node and the default scene both give. False: not one of them.
bool readShared(const JsonDocument& d, const JsonNode& m, LevelHints& h, Sink& sink) {
    if (m.key == "col") {
        if (const auto v = named<ecs::ColliderShape>(m, kShapes)) h.col = *v;
        else sink.add("E_LEVEL_EXTRAS", m.key, "is one of: " + listOf(kShapes) + "; got " + shown(d, m));
        return true;
    }
    if (m.key == "body") {
        if (const auto v = named<ecs::Motion>(m, kMotions)) h.body = *v;
        else sink.add("E_LEVEL_EXTRAS", m.key, "is one of: " + listOf(kMotions) + "; got " + shown(d, m));
        return true;
    }
    return false;
}

void readText(const JsonDocument& d, const JsonNode& m, std::size_t limit, std::string& out, Sink& sink) {
    if (m.type == JsonType::String && !m.text.empty() && codePoints(m.text) <= limit) out = std::string(m.text);
    else sink.add("E_LEVEL_EXTRAS", m.key, "is a string of 1 to " + std::to_string(limit) + " characters; got " + shown(d, m));
}

// A capitalised key: a saved component, read exactly as a scene file reads it.
void readComponent(const JsonDocument& d, const JsonNode& m, const reflect::TypeRegistry& types, LevelHints& h,
                   Sink& sink) {
    const reflect::TypeInfo* t = types.find(m.key);
    if (!t || t->role != reflect::TypeRole::Component) {
        std::vector<std::string_view> names;
        for (const reflect::TypeInfo* c : types.types())
            if (c->role == reflect::TypeRole::Component && c->serialized) names.push_back(c->name);
        sink.add("E_LEVEL_EXTRAS", m.key, "no component has this name (a capitalised key names a component)",
                 reflect::suggest(m.key, names));
        return;
    }
    if (!t->serialized || !ecs::componentOps(*t))
        return sink.add("E_LEVEL_EXTRAS", m.key, "is runtime state the engine makes; extras cannot give it");
    if (t->alwaysPresent || t == types.find<ecs::Parent>())
        return sink.add("E_LEVEL_EXTRAS", m.key, "comes from the node itself (its name, transform and place in the file)");
    if (t == types.find<ecs::Level>()) return sink.add("E_LEVEL_EXTRAS", m.key, "a level's node cannot hold a level");
    if (m.type != JsonType::Object) return sink.add("E_LEVEL_EXTRAS", m.key, "a component is an object; got " + shown(d, m));

    const std::align_val_t align{t->align ? t->align : alignof(std::max_align_t)};
    void* value = ::operator new(t->size, align);
    t->construct(value);
    reflect::DecodeContext ctx(d);
    ctx.component = t;
    reflect::readStruct(value, *t, m, ctx);
    t->destroy(value);
    ::operator delete(value, align);
    for (Diagnostic& e : ctx.errors) sink.add(std::move(e), m.key);
    for (Diagnostic& w : ctx.warnings) sink.add(std::move(w), m.key);
    if (!ctx.refs.empty())
        sink.add("E_LEVEL_EXTRAS", m.key, "names another entity; a level's extras cannot refer to entities");
    if (!ctx.errors.empty() || !ctx.refs.empty()) return;
    reflect::JsonWriter w;
    reflect::writeJson(w, d, m);
    h.components.emplace_back(t, w.take());
}

} // namespace

LevelHints parseHints(const GltfStructure& s, int node, const reflect::TypeRegistry& types, std::vector<Diagnostic>& out) {
    LevelHints h;
    if (node < 0 || node >= static_cast<int>(s.nodes.size())) return h;
    const GltfNode& n = s.nodes[static_cast<std::size_t>(node)];
    Sink sink{nodePath(s, node), out};

    // 1. The node's own extras.
    bool visGiven = false;
    if (!n.extrasJson.empty()) {
        auto parsed = ecs::parseJson(n.extrasJson);
        const JsonDocument* d = parsed ? &parsed.value() : nullptr;
        if (!d || d->root().type != JsonType::Object) {
            sink.add("W_LEVEL_EXTRAS", "", "extras is not a JSON object; ignored");
        } else {
            const JsonNode& root = d->root();
            for (uint32_t i = 0; i < root.count; ++i) {
                const JsonNode& m = d->child(root, i);
                if (readShared(*d, m, h, sink)) continue;
                const std::string_view key = m.key;
                if (key == "mass") {
                    const auto v = number(m);
                    if (v && *v > 0.0) h.mass = static_cast<float>(*v);
                    else sink.add("E_LEVEL_EXTRAS", key, "is a mass in kg, > 0; got " + shown(*d, m));
                } else if (key == "fric" || key == "rest") {
                    const auto v = number(m);
                    if (v && *v >= 0.0 && *v <= 1.0) (key == "fric" ? h.fric : h.rest) = static_cast<float>(*v);
                    else sink.add("E_LEVEL_EXTRAS", key, "is a number in [0, 1]; got " + shown(*d, m));
                } else if (key == "vis") {
                    const auto v = number(m);
                    if (v && (*v == 0.0 || *v == 1.0)) {
                        h.visible = *v != 0.0;
                        visGiven = true;
                    } else {
                        sink.add("E_LEVEL_EXTRAS", key, "is 0 (collision only) or 1; got " + shown(*d, m));
                    }
                } else if (key == "spawn") {
                    readText(*d, m, kMaxSpawn, h.spawn, sink);
                } else if (key == "socket") {
                    readText(*d, m, kMaxSocket, h.socket, sink);
                } else if (!key.empty() && key[0] >= 'A' && key[0] <= 'Z') {
                    readComponent(*d, m, types, h, sink);
                } else {
                    sink.add("W_LEVEL_EXTRAS", key, "not a level hint (" + listOf(kHintKeys) + "); ignored",
                             reflect::suggest(key, kHintKeys));
                }
            }
        }
    }

    // 2. Unreal's naming, where the extras said nothing.
    if (n.mesh >= 0 && n.name.starts_with("UCX_")) {
        if (!h.col) h.col = ecs::ColliderShape::Convex;
        if (!visGiven) h.visible = false;
    }
    if (n.mesh < 0 && n.name.starts_with("SOCKET_") && n.name.size() > 7 && h.socket.empty()) {
        const std::string name = n.name.substr(7);
        if (codePoints(name) <= kMaxSocket) h.socket = name;
        else sink.add("E_LEVEL_EXTRAS", "socket", "the name after SOCKET_ is over " + std::to_string(kMaxSocket) + " characters");
    }

    // 3. The default scene's col and body, for a node that sets neither; judged with node 0.
    if (!s.sceneExtrasJson.empty() && (!h.col || !h.body || node == 0)) {
        auto parsed = ecs::parseJson(s.sceneExtrasJson);
        if (parsed && parsed.value().root().type == JsonType::Object) {
            const JsonDocument& d = parsed.value();
            std::vector<Diagnostic> sceneDiags;
            Sink scene{"", sceneDiags};
            LevelHints defaults;
            for (uint32_t i = 0; i < d.root().count; ++i) readShared(d, d.child(d.root(), i), defaults, scene);
            if (!h.col) h.col = defaults.col;
            if (!h.body) h.body = defaults.body;
            if (node == 0) out.insert(out.end(), sceneDiags.begin(), sceneDiags.end());
        }
    }
    return h;
}

std::string extrasJsonSchema() {
    reflect::JsonWriter w(2048, reflect::JsonLayout::Pretty);
    const auto strings = [&](std::span<const std::string_view> names) {
        w.beginArray();
        for (const auto n : names) w.string(n);
        w.endArray();
    };
    w.beginObject();
    w.key("$schema"); w.string("https://json-schema.org/draft/2020-12/schema");
    w.key("$id"); w.string("https://ohao.dev/schema/level-extras.json");
    w.key("title"); w.string("OHAO level hints: a glTF node's extras");
    w.key("description");
    w.string("docs/specs/2026-10-26-level-pipeline-design.md, 'Extras: the hint schema'. Other keys are allowed here: "
             "component passthrough (a registered component's name with an object value) and other tools' properties "
             "are checked by the engine's level loader.");
    w.key("type"); w.string("object");
    w.key("properties");
    w.beginObject();
    w.key("col");
    w.beginObject();
    w.key("enum"); strings(kShapes);
    w.key("description"); w.string("Collision shape fitted to the node's mesh; default mesh on mesh nodes, none elsewhere");
    w.endObject();
    w.key("body");
    w.beginObject();
    w.key("enum"); strings(kMotions);
    w.key("description"); w.string("Motion type; default static");
    w.endObject();
    w.key("mass");
    w.beginObject();
    w.key("type"); w.string("number");
    w.key("exclusiveMinimum"); w.int64(0);
    w.key("description"); w.string("kg; dynamic bodies, default 10");
    w.endObject();
    for (const auto& [key, what] : {std::pair{"fric", "Friction, default 0.6"}, std::pair{"rest", "Restitution, default 0"}}) {
        w.key(key);
        w.beginObject();
        w.key("type"); w.string("number");
        w.key("minimum"); w.int64(0);
        w.key("maximum"); w.int64(1);
        w.key("description"); w.string(what);
        w.endObject();
    }
    w.key("vis");
    w.beginObject();
    w.key("enum");
    w.beginArray();
    w.int64(0);
    w.int64(1);
    w.endArray();
    w.key("description"); w.string("0: collision only, not drawn");
    w.endObject();
    for (const auto& [key, limit, what] : {std::tuple{"spawn", kMaxSpawn, "SpawnPoint tag (player, enemy, ...)"},
                                           std::tuple{"socket", kMaxSocket, "Socket name (an attach point)"}}) {
        w.key(key);
        w.beginObject();
        w.key("type"); w.string("string");
        w.key("minLength"); w.int64(1);
        w.key("maxLength"); w.int64(static_cast<int64_t>(limit));
        w.key("description"); w.string(what);
        w.endObject();
    }
    w.endObject();
    w.endObject();
    std::string out = w.take();
    out.push_back('\n');
    return out;
}

} // namespace ohao::level
