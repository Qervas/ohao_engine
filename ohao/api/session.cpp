#include "api/internal.hpp"

#include "ecs/components.hpp"
#include "ecs/scene_io.hpp"
#include "reflect/schema.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ohao::api {

namespace {

constexpr reflect::MethodDoc kMethods[] = {
    {"schema.get", "{}", "the catalogue (ohao_schema.json), compact",
     "Every component, field, enum, limit, unit, error code and method. Read once; key on schemaHash."},
    {"scene.validate", "{text | path | scene, limit = 20}", "{ok, errors?, warnings?, upgraded?, more?}",
     "Checks a scene exactly as scene.load would, without touching the world."},
    {"scene.load", "{text | path | scene, seed?}", "{entities, preserved?, upgraded?, warnings?}",
     "Loads a scene into the session's world, which must be empty. All or nothing. seed (16 hex digits) "
     "fixes the id generator for later creates."},
    {"scene.save", "{path?, layout = compact | pretty, defaults = false}", "{scene} or {path, bytes}",
     "The canonical scene. Without a path it is returned as an object (compact)."},
    {"world.info", "{}", "{entities, frame, schemaHash, idCounter}", "Cheap orientation: sizes and identity."},
    {"world.summary", "{depth = 2, maxChildren = 8, maxNodes = 64}",
     "{entities, roots, components, bounds?, outline, truncated?}",
     "Counts per component, world-space bounds of entity origins, and a capped hierarchy outline."},
    {"world.query",
     "{ids?, names?, nameContains?, has?, fields?, limit = 50, offset = 0, defaults = false}",
     "{count, entities, missing?, next?}",
     "Entities in creation order, filtered (all filters must match) and projected. fields: \"/Type\" or "
     "\"/Type/field\"; without fields each entity is its file form."},
    {"world.apply", "{commands}", "{applied, results, inverse}",
     "One atomic batch of commands, applied in order; on any error nothing changes. inverse undoes the batch."},
    {"world.step", "{frames = 1, dt = 1/60}", "{frame}", "Runs the world's systems with a fixed dt."},
    {"world.snapshot", "{name}", "{name, entities, bytes}",
     "Captures the world in memory under a name (replacing one with that name): ids, hierarchy, every saved "
     "component, the id generator and the frame."},
    {"world.restore", "{name}", "{created, destroyed, changed, frame}",
     "Brings the world back to a snapshot, writing only what differs. The snapshot is kept for further restores."},
};

constexpr std::size_t kMaxErrorData = 20;   // diagnostics per error envelope

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    ok = static_cast<bool>(f);
    if (!ok) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeUpgraded(JsonWriter& w, const std::vector<ecs::UpgradeApplied>& up) {
    w.beginArray();
    for (const auto& u : up) {
        w.beginObject();
        w.key("component"); w.string(u.component);
        w.key("from"); w.int64(u.from);
        w.key("to"); w.int64(u.to);
        w.key("count"); w.uint64(u.count);
        w.endObject();
    }
    w.endArray();
}

void writeDiagnostics(JsonWriter& w, const std::vector<Diagnostic>& ds, std::size_t limit) {
    w.beginArray();
    for (std::size_t i = 0; i < ds.size() && i < limit; ++i) reflect::writeDiagnostic(w, ds[i]);
    w.endArray();
}

// The scene text from {text | path | scene}. False after reporting.
bool sceneSource(const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx, std::string& text) {
    const JsonNode* t = doc.member(params, "text");
    const JsonNode* p = doc.member(params, "path");
    const JsonNode* s = doc.member(params, "scene");
    if ((t != nullptr) + (p != nullptr) + (s != nullptr) != 1) {
        ctx.error("E_COMMAND", params, "give exactly one of \"text\", \"path\" or \"scene\"",
                  "{\"text\": \"...\"} or {\"path\": \"...\"} or {\"scene\": {...}}");
        return false;
    }
    if (s) {
        JsonWriter w(s->count * 64);
        reflect::writeJson(w, doc, *s);
        text = w.take();
        return true;
    }
    const JsonNode& n = t ? *t : *p;
    ctx.push(t ? "text" : "path");
    if (n.type != JsonType::String) {
        ctx.error("E_TYPE", n, "expected a string", "string", ctx.gotText(n));
        ctx.pop();
        return false;
    }
    if (t) {
        text = std::string(n.text);
    } else {
        bool ok = false;
        text = readFile(std::string(n.text), ok);
        if (!ok) {
            ctx.error("E_IO", n, "cannot read " + std::string(n.text));
            ctx.pop();
            return false;
        }
    }
    ctx.pop();
    return true;
}

// Moves scene diagnostics (paths into the scene, not the params) into the request's errors.
void reportAll(DecodeContext& ctx, std::vector<Diagnostic>& ds) {
    for (Diagnostic& d : ds) ctx.report(std::move(d));
}

// ---- methods --------------------------------------------------------------------------

bool schemaGet(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
               JsonWriter& out) {
    if (!checkKeys(doc, params, {}, "schema.get params", ctx)) return false;
    if (s.catalogue.empty())
        s.catalogue = reflect::exportCatalogue(s.types, {kMethods, reflect::JsonLayout::Compact});
    out.raw(s.catalogue);
    return true;
}

bool sceneValidate(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                   JsonWriter& out) {
    if (!checkKeys(doc, params, {"text", "path", "scene", "limit"}, "scene.validate params", ctx)) return false;
    int64_t limit = 20;
    if (!optInt(doc, params, "limit", ctx, 0, 100, limit)) return false;
    std::string text;
    if (!sceneSource(doc, params, ctx, text)) return false;
    const ecs::ValidationReport r = ecs::validateScene(text, s.types);
    const auto n = static_cast<std::size_t>(limit);
    out.beginObject();
    out.key("ok"); out.boolean(r.ok());
    if (!r.errors.empty()) { out.key("errors"); writeDiagnostics(out, r.errors, n); }
    if (!r.warnings.empty()) { out.key("warnings"); writeDiagnostics(out, r.warnings, n); }
    if (!r.upgraded.empty()) { out.key("upgraded"); writeUpgraded(out, r.upgraded); }
    const std::size_t cut = (r.errors.size() > n ? r.errors.size() - n : 0) +
                            (r.warnings.size() > n ? r.warnings.size() - n : 0);
    if (cut) { out.key("more"); out.uint64(cut); }
    out.endObject();
    return true;
}

bool sceneLoad(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
               JsonWriter& out) {
    if (!checkKeys(doc, params, {"text", "path", "scene", "seed"}, "scene.load params", ctx)) return false;
    uint64_t seed = 0;
    const JsonNode* seedNode = doc.member(params, "seed");
    if (seedNode) {
        ctx.push("seed");
        const bool ok = reflect::readHex64(*seedNode, ctx, "E_TYPE", seed);
        ctx.pop();
        if (!ok) return false;
    }
    std::string text;
    if (!sceneSource(doc, params, ctx, text)) return false;
    auto r = ecs::loadScene(s.world, text, s.types);
    if (!r) {
        reportAll(ctx, r.error());
        return false;
    }
    if (seedNode) s.world.seedIds(seed);
    const ecs::LoadReport& rep = r.value();
    out.beginObject();
    out.key("entities"); out.uint64(rep.entities);
    if (rep.preservedUnknown) { out.key("preserved"); out.uint64(rep.preservedUnknown); }
    if (!rep.upgraded.empty()) { out.key("upgraded"); writeUpgraded(out, rep.upgraded); }
    if (!rep.warnings.empty()) { out.key("warnings"); writeDiagnostics(out, rep.warnings, kMaxErrorData); }
    out.endObject();
    return true;
}

bool sceneSave(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
               JsonWriter& out) {
    if (!checkKeys(doc, params, {"path", "layout", "defaults"}, "scene.save params", ctx)) return false;
    std::string_view path, layout;
    bool defaults = false;
    if (!optString(doc, params, "path", ctx, path) || !optString(doc, params, "layout", ctx, layout) ||
        !optBool(doc, params, "defaults", ctx, defaults))
        return false;
    ecs::SaveOptions o;
    o.includeDefaults = defaults;
    o.layout = path.empty() ? reflect::JsonLayout::Compact : reflect::JsonLayout::Pretty;
    if (!layout.empty()) {
        if (layout != "compact" && layout != "pretty") {
            ctx.push("layout");
            ctx.error("E_ENUM", *doc.member(params, "layout"), "unknown layout", "one of: compact, pretty",
                      "\"" + std::string(layout) + "\"");
            ctx.pop();
            return false;
        }
        if (path.empty() && layout == "pretty") {
            ctx.error("E_COMMAND", params, "an inline scene is always compact; give a path for pretty");
            return false;
        }
        o.layout = layout == "compact" ? reflect::JsonLayout::Compact : reflect::JsonLayout::Pretty;
    }
    auto text = ecs::saveScene(s.world, s.types, o);
    if (!text) {
        ctx.report(std::move(text.error()));
        return false;
    }
    out.beginObject();
    if (path.empty()) {
        out.key("scene");
        out.raw(text.value());
    } else {
        std::ofstream f(std::string(path), std::ios::binary | std::ios::trunc);
        f.write(text.value().data(), static_cast<std::streamsize>(text.value().size()));
        if (!f) {
            ctx.push("path");
            ctx.error("E_IO", *doc.member(params, "path"), "cannot write " + std::string(path));
            ctx.pop();
            return false;
        }
        out.key("path"); out.string(path);
        out.key("bytes"); out.uint64(text.value().size());
    }
    out.endObject();
    return true;
}

bool worldInfo(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
               JsonWriter& out) {
    if (!checkKeys(doc, params, {}, "world.info params", ctx)) return false;
    out.beginObject();
    out.key("entities"); out.uint64(s.world.idCount());
    out.key("frame"); out.uint64(s.frame);
    out.key("schemaHash"); out.hex64(s.schemaHash);
    out.key("idCounter"); out.uint64(s.world.idGenState().counter);
    out.endObject();
    return true;
}

bool worldStep(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
               JsonWriter& out) {
    if (!checkKeys(doc, params, {"frames", "dt"}, "world.step params", ctx)) return false;
    int64_t frames = 1;
    if (!optInt(doc, params, "frames", ctx, 0, 1'000'000, frames)) return false;
    float dt = 1.0f / 60.0f;
    if (const JsonNode* n = doc.member(params, "dt")) {
        ctx.push("dt");
        const bool ok = reflect::readFloat(*n, ctx, dt);
        if (ok && !(dt > 0.0f && dt <= 1.0f))
            ctx.error("E_RANGE", *n, "dt is out of range", "a step in (0, 1] seconds", std::string(n->text));
        ctx.pop();
        if (!ctx.errors.empty()) return false;
    }
    for (int64_t i = 0; i < frames; ++i) {
        s.world.update(dt);
        ++s.frame;
    }
    out.beginObject();
    out.key("frame"); out.uint64(s.frame);
    out.endObject();
    return true;
}

// The snapshot name from {name}. False after reporting.
bool snapshotName(const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx, std::string_view what,
                  std::string_view& name) {
    if (!checkKeys(doc, params, {"name"}, what, ctx) || !optString(doc, params, "name", ctx, name)) return false;
    if (name.empty()) {
        ctx.error("E_COMMAND", params, std::string(what) + " needs a non-empty \"name\"", "{\"name\": \"base\"}");
        return false;
    }
    return true;
}

bool worldSnapshot(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                   JsonWriter& out) {
    std::string_view name;
    if (!snapshotName(doc, params, ctx, "world.snapshot params", name)) return false;
    auto it = s.snapshots.find(name);
    if (it == s.snapshots.end()) it = s.snapshots.emplace(std::string(name), ecs::Snapshot{}).first;
    ecs::takeSnapshot(s.world, s.types, it->second, s.frame);   // reuses the old snapshot's memory
    const std::size_t rows = it->second.rows(), bytes = it->second.bytes();
    out.beginObject();
    out.key("name"); out.string(name);
    out.key("entities"); out.uint64(rows);
    out.key("bytes"); out.uint64(bytes);
    out.endObject();
    return true;
}

bool worldRestore(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                  JsonWriter& out) {
    std::string_view name;
    if (!snapshotName(doc, params, ctx, "world.restore params", name)) return false;
    const auto it = s.snapshots.find(name);
    if (it == s.snapshots.end()) {
        std::vector<std::string_view> names;
        for (const auto& [n, snap] : s.snapshots) names.push_back(n);
        ctx.push("name");
        ctx.error("E_SNAPSHOT_UNKNOWN", *doc.member(params, "name"), "no snapshot is named '" + std::string(name) + "'",
                  names.empty() ? "a name given to world.snapshot (none yet)" : reflect::oneOf(names),
                  "\"" + std::string(name) + "\"", reflect::suggest(name, names));
        ctx.pop();
        return false;
    }
    const ecs::RestoreStats st = ecs::restoreSnapshot(s.world, it->second);
    s.frame = it->second.frame;
    out.beginObject();
    out.key("created"); out.uint64(st.created);
    out.key("destroyed"); out.uint64(st.destroyed);
    out.key("changed"); out.uint64(st.changed);
    out.key("frame"); out.uint64(s.frame);
    out.endObject();
    return true;
}

struct MethodEntry {
    std::string_view name;
    MethodFn fn;
};
constexpr MethodEntry kDispatch[] = {
    {"schema.get", schemaGet},       {"scene.validate", sceneValidate}, {"scene.load", sceneLoad},
    {"scene.save", sceneSave},       {"world.info", worldInfo},         {"world.summary", methodSummary},
    {"world.query", methodQuery},    {"world.apply", methodApply},      {"world.step", worldStep},
    {"world.snapshot", worldSnapshot}, {"world.restore", worldRestore},
};
static_assert(std::size(kDispatch) == std::size(kMethods));

std::string errorEnvelope(std::vector<Diagnostic>& errors) {
    if (errors.empty()) {
        Diagnostic d;
        d.code = "E_COMMAND";
        d.message = "the request failed without a diagnostic";
        errors.push_back(std::move(d));
    }
    JsonWriter w(256);
    w.beginObject();
    w.key("error");
    w.beginObject();
    w.key("code"); w.string(errors.front().code);
    w.key("message");
    w.string(errors.size() > 1 ? errors.front().message + " (+" + std::to_string(errors.size() - 1) + " more)"
                               : errors.front().message);
    w.key("data");
    writeDiagnostics(w, errors, kMaxErrorData);
    w.endObject();
    w.endObject();
    return w.take();
}

} // namespace

std::span<const reflect::MethodDoc> methods() noexcept { return kMethods; }

// ---- helpers --------------------------------------------------------------------------

std::string hex16(uint64_t id) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, id >>= 4) s[static_cast<std::size_t>(i)] = kDigits[id & 15u];
    return s;
}

bool checkKeys(const JsonDocument& doc, const JsonNode& obj, std::initializer_list<std::string_view> allowed,
               std::string_view what, DecodeContext& ctx) {
    const std::size_t before = ctx.errors.size();
    for (uint32_t i = 0; i < obj.count; ++i) {
        const JsonNode& m = doc.child(obj, i);
        const bool known = std::find(allowed.begin(), allowed.end(), m.key) != allowed.end();
        ctx.push(m.key);
        if (!known) {
            const std::span<const std::string_view> names(allowed.begin(), allowed.size());
            ctx.error("E_UNKNOWN_FIELD", m, "unknown member '" + std::string(m.key) + "' in " + std::string(what),
                      names.empty() ? "no members" : reflect::oneOf(names), "\"" + std::string(m.key) + "\"",
                      reflect::suggest(m.key, names));
        } else {
            for (uint32_t j = 0; j < i; ++j)
                if (doc.child(obj, j).key == m.key) {
                    ctx.error("E_DUPLICATE_KEY", m, "\"" + std::string(m.key) + "\" is given twice");
                    break;
                }
        }
        ctx.pop();
    }
    return ctx.errors.size() == before;
}

bool optBool(const JsonDocument& doc, const JsonNode& obj, std::string_view key, DecodeContext& ctx, bool& out) {
    const JsonNode* n = doc.member(obj, key);
    if (!n) return true;
    if (n->type != JsonType::Bool) {
        ctx.push(key);
        ctx.error("E_TYPE", *n, "expected true or false", "boolean", ctx.gotText(*n));
        ctx.pop();
        return false;
    }
    out = n->boolean;
    return true;
}

bool optInt(const JsonDocument& doc, const JsonNode& obj, std::string_view key, DecodeContext& ctx, int64_t lo,
            int64_t hi, int64_t& out) {
    const JsonNode* n = doc.member(obj, key);
    if (!n) return true;
    ctx.push(key);
    const bool ok = reflect::readInt(*n, ctx, lo, hi, out);
    ctx.pop();
    return ok;
}

bool optString(const JsonDocument& doc, const JsonNode& obj, std::string_view key, DecodeContext& ctx,
               std::string_view& out) {
    const JsonNode* n = doc.member(obj, key);
    if (!n) return true;
    if (n->type != JsonType::String) {
        ctx.push(key);
        ctx.error("E_TYPE", *n, "expected a string", "string", ctx.gotText(*n));
        ctx.pop();
        return false;
    }
    out = n->text;
    return true;
}

const TypeInfo* findComponent(Session::Impl& s, std::string_view name, const JsonNode& at, bool savedOnly,
                              DecodeContext& ctx) {
    const auto it = s.byName.find(name);
    if (it != s.byName.end() && (!savedOnly || it->second->serialized)) return it->second;
    if (it != s.byName.end()) {
        ctx.error("E_COMPONENT_UNKNOWN", at, "'" + std::string(name) + "' is runtime state; commands cannot set it",
                  "a saved component name", "\"" + std::string(name) + "\"");
        return nullptr;
    }
    std::vector<std::string_view> names;
    if (savedOnly) names = s.savedNames;
    else
        for (const TypeInfo* t : s.withOps) names.push_back(t->name);
    ctx.error("E_COMPONENT_UNKNOWN", at, "no component is named '" + std::string(name) + "'", reflect::oneOf(names),
              "\"" + std::string(name) + "\"", reflect::suggest(name, names));
    return nullptr;
}

void writeComponents(Session::Impl& s, JsonWriter& w, entt::entity e, bool includeDefaults, bool preserved) {
    const entt::registry& reg = s.world.registry();
    for (const TypeInfo* t : s.saved) {
        const void* v = ecs::componentOps(*t)->tryGet(reg, e);
        if (!v) continue;
        if (!includeDefaults && t->alwaysPresent && reflect::equalStruct(v, t->defaultValue.get(), *t)) continue;
        w.key(t->name);
        reflect::writeStruct(v, w, *t, includeDefaults ? nullptr : t->defaultValue.get());
    }
    if (!preserved) return;
    if (const auto* p = reg.try_get<ecs::PreservedComponents>(e)) {
        std::vector<const ecs::PreservedComponent*> sorted;
        for (const auto& entry : p->entries) sorted.push_back(&entry);
        std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->name < b->name; });
        for (const auto* entry : sorted) {
            w.key(entry->name);
            w.raw(entry->json);
        }
    }
}

// ---- Session --------------------------------------------------------------------------

Session::Impl::Impl(ecs::World& w, const reflect::TypeRegistry& t) : world(w), types(t) {
    for (const TypeInfo* ti : t.types()) {
        if (ti->role != reflect::TypeRole::Component || !ecs::componentOps(*ti)) continue;
        byName.emplace(ti->name, ti);
        withOps.push_back(ti);
        if (ti->serialized) {
            saved.push_back(ti);
            savedNames.push_back(ti->name);
        }
    }
    tName = t.find<ecs::Name>();
    tTransform = t.find<ecs::Transform>();
    tParent = t.find<ecs::Parent>();
    schemaHash = reflect::schemaHash(t, kMethods);
}

Session::Session(ecs::World& world, const reflect::TypeRegistry& types)
    : m(std::make_unique<Impl>(world, types)) {}
Session::~Session() = default;

ecs::World& Session::world() noexcept { return m->world; }
uint64_t Session::frame() const noexcept { return m->frame; }

std::string Session::call(std::string_view method, std::string_view params) {
    const auto found = std::find_if(std::begin(kDispatch), std::end(kDispatch),
                                    [&](const MethodEntry& e) { return e.name == method; });
    if (found == std::end(kDispatch)) {
        std::vector<std::string_view> names;
        for (const auto& e : kDispatch) names.push_back(e.name);
        std::vector<Diagnostic> errs(1);
        errs[0].code = "E_METHOD";
        errs[0].message = "no method is named '" + std::string(method) + "'";
        errs[0].expected = reflect::oneOf(names);
        errs[0].got = "\"" + std::string(method) + "\"";
        errs[0].suggestion = reflect::suggest(method, names);
        return errorEnvelope(errs);
    }
    auto parsed = m->paramsParser.parse(params.empty() ? std::string_view("{}") : params);
    if (!parsed) {
        std::vector<Diagnostic> errs{std::move(parsed.error())};
        return errorEnvelope(errs);
    }
    const JsonDocument& doc = *parsed.value();
    DecodeContext ctx(doc);
    if (doc.root().type != JsonType::Object) {
        ctx.error("E_TYPE", doc.root(), "params is an object", "object", ctx.gotText(doc.root()));
        return errorEnvelope(ctx.errors);
    }
    // The result is written inside the envelope directly; on failure it is discarded.
    ecs::WorldEntityIds ids(m->world);
    JsonWriter out(256);
    out.entityIds = &ids;
    out.beginObject();
    out.key("result");
    if (!found->fn(*m, doc, doc.root(), ctx, out) || !ctx.errors.empty()) return errorEnvelope(ctx.errors);
    out.endObject();
    return out.take();
}

} // namespace ohao::api
