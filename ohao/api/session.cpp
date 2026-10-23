#include "api/internal.hpp"

#include "core/log.hpp"
#include "ecs/components.hpp"
#include "ecs/scene_io.hpp"
#include "reflect/schema.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ohao::api {

namespace {

constexpr std::size_t kMaxErrorData = 20;   // diagnostics per error envelope

// ---- the method table -------------------------------------------------------------------

std::vector<reflect::MethodDoc> buildMethods() {
    const reflect::TypeRegistry& p = paramsRegistry();
    return {
        {"engine.info", p.find<EngineInfoParams>(), "{protocolVersion, schemaHash, version, host, time}",
         "The native handshake: protocol version, schemaHash (key cached catalogues on it), engine version, host "
         "and time state."},
        {"schema.get", p.find<SchemaGetParams>(), "the catalogue (ohao_schema.json), compact",
         "Every component, field, enum, limit, unit, error code and method. Read once; key on schemaHash."},
        {"scene.validate", p.find<SceneValidateParams>(), "{ok, errors?, warnings?, upgraded?, more?}",
         "Checks a scene exactly as scene.load would, without touching the world. checkAssets also reports model "
         "files that do not exist (E_ASSET_NOT_FOUND)."},
        {"scene.load", p.find<SceneLoadParams>(), "{entities, preserved?, upgraded?, warnings?}",
         "Loads a scene into the world, which must be empty unless replace is true (then it is cleared first; "
         "snapshots are kept). All or nothing. seed fixes the id generator for later creates."},
        {"scene.save", p.find<SceneSaveParams>(), "{scene} or {path, bytes}",
         "The canonical scene. Without a path it is returned as an object (compact)."},
        {"world.info", p.find<WorldInfoParams>(), "{entities, frame, schemaHash, idCounter, host}",
         "Cheap orientation: sizes and identity."},
        {"world.summary", p.find<WorldSummaryParams>(), "{entities, roots, components, bounds?, outline, moreRoots?}",
         "Counts per component, world-space bounds of entity origins, and a capped hierarchy outline. A node whose "
         "children are not all listed carries more (the count left out); moreRoots counts unlisted roots."},
        {"world.query", p.find<WorldQueryParams>(), "{count, entities, missing?, next?}",
         "Entities in creation order, filtered (all filters must match) and projected. fields: \"/Type\" or "
         "\"/Type/field\"; without fields each entity is its file form."},
        {"world.apply", p.find<WorldApplyParams>(), "{applied, results, inverse}",
         "One atomic batch of commands, applied in order; on any error nothing changes. inverse undoes the batch."},
        {"world.step", p.find<WorldStepParams>(), "{frame}",
         "Runs the world's systems for frames fixed steps. Cancellable between steps."},
        {"world.snapshot", p.find<WorldSnapshotParams>(), "{name, entities, bytes}",
         "Captures the world in memory under a name (replacing one with that name): ids, hierarchy, every saved "
         "component, the id generator and the frame."},
        {"world.restore", p.find<WorldRestoreParams>(), "{created, destroyed, changed, frame}",
         "Brings the world back to a snapshot, writing only what differs. The snapshot is kept for further restores."},
        {"render.capture", p.find<RenderCaptureParams>(), "{path, frames, ms, validation?}",
         "Renders the world through its Camera and Environment to a PNG. Needs a host with a GPU; the renderer "
         "starts on first use. post turns on deferred effects."},
        {"log.read", p.find<LogReadParams>(), "{lines: [{seq, t, level, channel, msg, repeat?}], next, dropped?}",
         "Engine log records after since (pass the last read's next), filtered by level and channel."},
    };
}

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
bool sceneSource(Session::Impl& s, const JsonNode& params, const std::optional<std::string>& text,
                 const std::optional<std::string>& path, const reflect::JsonRef& scene, DecodeContext& ctx,
                 std::string& out) {
    if (text.has_value() + path.has_value() + static_cast<bool>(scene) != 1) {
        ctx.error("E_COMMAND", params, "give exactly one of \"text\", \"path\" or \"scene\"",
                  "{\"text\": \"...\"} or {\"path\": \"...\"} or {\"scene\": {...}}");
        return badParams(s);
    }
    if (scene) {
        JsonWriter w(scene.node->count * 64);
        reflect::writeJson(w, *scene.doc, *scene.node);
        out = w.take();
        return true;
    }
    if (text) {
        out = *text;
        return true;
    }
    std::string file;
    if (!usePath(s, params, "path", *path, PathUse::Read, ctx, file)) return false;
    bool ok = false;
    out = readFile(file, ok);
    if (!ok) {
        ctx.push("path");
        ctx.error("E_IO", *ctx.doc.member(params, "path"), "cannot read " + *path);
        ctx.pop();
        return false;
    }
    return true;
}

// Moves scene diagnostics (paths into the scene, not the params) into the request's errors.
void reportAll(DecodeContext& ctx, std::vector<Diagnostic>& ds) {
    for (Diagnostic& d : ds) ctx.report(std::move(d));
}

void writeTime(Session::Impl& s, JsonWriter& out) {
    // Headless and CLI sessions are lockstep only (sub-project 4 spec, "Time modes").
    out.beginObject();
    out.key("mode"); out.string("lockstep");
    out.key("paused"); out.boolean(false);
    out.key("frame"); out.uint64(s.frame);
    out.key("fixedDt"); out.float32(1.0f / 60.0f);
    out.endObject();
}

// ---- methods --------------------------------------------------------------------------

bool engineInfo(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    EngineInfoParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    out.beginObject();
    out.key("protocolVersion"); out.int64(kProtocolVersion);
    out.key("schemaHash"); out.hex64(s.schemaHash);
    out.key("version"); out.string(kEngineVersion);
    out.key("host"); out.string(hostKindName(s.host));
    out.key("time"); writeTime(s, out);
    out.endObject();
    return true;
}

bool schemaGet(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    SchemaGetParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (s.catalogue.empty())
        s.catalogue = reflect::exportCatalogue(s.types, {methods(), reflect::JsonLayout::Compact});
    out.raw(s.catalogue);
    return true;
}

bool sceneValidate(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                   JsonWriter& out) {
    SceneValidateParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (p.checkAssets && !s.assets) {
        ctx.error("E_COMMAND", params, "this session has no asset resolver, so it cannot check assets");
        return false;
    }
    std::string text;
    if (!sceneSource(s, params, p.text, p.path, p.scene, ctx, text)) return false;
    const ecs::ValidationReport r = ecs::validateScene(text, s.types, p.checkAssets ? s.assets : nullptr);
    const auto n = static_cast<std::size_t>(p.limit);
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

bool sceneLoad(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    SceneLoadParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    std::string text;
    if (!sceneSource(s, params, p.text, p.path, p.scene, ctx, text)) return false;
    if (p.replace) {
        // Checked first, so a scene that does not load leaves the world as it was.
        const ecs::ValidationReport r = ecs::validateScene(text, s.types, nullptr);
        if (!r.ok()) {
            auto errors = r.errors;
            reportAll(ctx, errors);
            return false;
        }
        std::vector<entt::entity> roots;
        const entt::registry& reg = s.world.registry();
        for (auto [e, id] : reg.view<const ecs::EntityId>().each())
            if (!reg.all_of<ecs::Parent>(e)) roots.push_back(e);
        for (const entt::entity e : roots) s.world.destroy(e);   // each subtree
        s.frame = 0;
    }
    auto r = ecs::loadScene(s.world, text, s.types);
    if (!r) {
        reportAll(ctx, r.error());
        return false;
    }
    if (p.seed) s.world.seedIds(*p.seed);
    const ecs::LoadReport& rep = r.value();
    out.beginObject();
    out.key("entities"); out.uint64(rep.entities);
    if (rep.preservedUnknown) { out.key("preserved"); out.uint64(rep.preservedUnknown); }
    if (!rep.upgraded.empty()) { out.key("upgraded"); writeUpgraded(out, rep.upgraded); }
    if (!rep.warnings.empty()) { out.key("warnings"); writeDiagnostics(out, rep.warnings, kMaxErrorData); }
    out.endObject();
    return true;
}

bool sceneSave(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    SceneSaveParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    ecs::SaveOptions o;
    o.includeDefaults = p.defaults;
    o.layout = p.path ? reflect::JsonLayout::Pretty : reflect::JsonLayout::Compact;
    if (p.layout) {
        if (!p.path && *p.layout == SaveLayout::pretty) {
            ctx.error("E_COMMAND", params, "an inline scene is always compact; give a path for pretty");
            return badParams(s);
        }
        o.layout = *p.layout == SaveLayout::compact ? reflect::JsonLayout::Compact : reflect::JsonLayout::Pretty;
    }
    std::string file;
    if (p.path && !usePath(s, params, "path", *p.path, PathUse::Write, ctx, file)) return false;
    auto text = ecs::saveScene(s.world, s.types, o);
    if (!text) {
        ctx.report(std::move(text.error()));
        return false;
    }
    out.beginObject();
    if (!p.path) {
        out.key("scene");
        out.raw(text.value());
    } else {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f.write(text.value().data(), static_cast<std::streamsize>(text.value().size()));
        if (!f) {
            ctx.push("path");
            ctx.error("E_IO", *ctx.doc.member(params, "path"), "cannot write " + *p.path);
            ctx.pop();
            return false;
        }
        out.key("path"); out.string(*p.path);
        out.key("bytes"); out.uint64(text.value().size());
    }
    out.endObject();
    return true;
}

bool worldInfo(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    WorldInfoParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    out.beginObject();
    out.key("entities"); out.uint64(s.world.idCount());
    out.key("frame"); out.uint64(s.frame);
    out.key("schemaHash"); out.hex64(s.schemaHash);
    out.key("idCounter"); out.uint64(s.world.idGenState().counter);
    out.key("host"); out.string(hostKindName(s.host));
    out.endObject();
    return true;
}

bool worldStep(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    WorldStepParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const float dt = p.dt.value_or(1.0f / 60.0f);
    if (!(dt > 0.0f)) {
        ctx.push("dt");
        ctx.error("E_RANGE", *ctx.doc.member(params, "dt"), "dt is out of range", "a step in (0, 1] seconds",
                  ctx.gotText(*ctx.doc.member(params, "dt")));
        ctx.pop();
        return badParams(s);
    }
    const auto frames = static_cast<uint64_t>(p.frames);
    for (uint64_t i = 0; i < frames; ++i) {
        if (s.cancelRequested()) {   // between whole steps
            s.cancelled = true;
            return false;
        }
        s.world.update(dt);
        ++s.frame;
        if (s.control && s.control->progress) s.control->progress(i + 1, frames);
    }
    out.beginObject();
    out.key("frame"); out.uint64(s.frame);
    out.endObject();
    return true;
}

// A snapshot name: non-empty. False after reporting.
bool snapshotName(Session::Impl& s, const JsonNode& params, const std::string& name, std::string_view what,
                  DecodeContext& ctx) {
    if (!name.empty()) return true;
    ctx.error("E_COMMAND", params, std::string(what) + " needs a non-empty \"name\"", "{\"name\": \"base\"}");
    return badParams(s);
}

bool worldSnapshot(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                   JsonWriter& out) {
    WorldSnapshotParams p;
    if (!decodeParams(s, params, ctx, p) || !snapshotName(s, params, p.name, "world.snapshot", ctx)) return false;
    auto it = s.snapshots.find(p.name);
    if (it == s.snapshots.end()) it = s.snapshots.emplace(p.name, ecs::Snapshot{}).first;
    ecs::takeSnapshot(s.world, s.types, it->second, s.frame);   // reuses the old snapshot's memory
    const std::size_t rows = it->second.rows(), bytes = it->second.bytes();
    out.beginObject();
    out.key("name"); out.string(p.name);
    out.key("entities"); out.uint64(rows);
    out.key("bytes"); out.uint64(bytes);
    out.endObject();
    return true;
}

bool worldRestore(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx,
                  JsonWriter& out) {
    WorldRestoreParams p;
    if (!decodeParams(s, params, ctx, p) || !snapshotName(s, params, p.name, "world.restore", ctx)) return false;
    const auto it = s.snapshots.find(p.name);
    if (it == s.snapshots.end()) {
        std::vector<std::string_view> names;
        for (const auto& [n, snap] : s.snapshots) names.push_back(n);
        ctx.push("name");
        ctx.error("E_SNAPSHOT_UNKNOWN", *doc.member(params, "name"), "no snapshot is named '" + p.name + "'",
                  names.empty() ? "a name given to world.snapshot (none yet)" : reflect::oneOf(names),
                  "\"" + p.name + "\"", reflect::suggest(p.name, names));
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

bool renderCapture(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                   JsonWriter& out) {
    RenderCaptureParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (p.path.empty()) {
        ctx.error("E_COMMAND", params, "render.capture needs a \"path\" for the PNG", "{\"path\": \"out.png\"}");
        return badParams(s);
    }
    if (p.exposure && !(*p.exposure > 0.0f)) {
        ctx.push("exposure");
        ctx.error("E_RANGE", *ctx.doc.member(params, "exposure"), "exposure is out of range", "in (0, 64]",
                  ctx.gotText(*ctx.doc.member(params, "exposure")));
        ctx.pop();
        return badParams(s);
    }
    if (!s.render) {
        ctx.error("E_COMMAND", params, "this session has no renderer; render.capture needs a host with a GPU (ohao run)");
        return false;
    }
    std::string file;
    if (!usePath(s, params, "path", p.path, PathUse::Write, ctx, file)) return false;
    static constexpr const char* kModes[] = {"deferred", "rt_realtime", "rt_offline"};
    static constexpr const char* kDenoisers[] = {"none", "oidn", "nrd", "atrous", "dlss"};
    static constexpr const char* kEffects[] = {"bloom", "taa", "ssao"};
    RenderRequest r;
    r.path = file;
    r.mode = kModes[static_cast<int>(p.mode)];
    r.spp = p.spp;
    r.frames = p.frames;
    r.width = static_cast<uint32_t>(p.width);
    r.height = static_cast<uint32_t>(p.height);
    r.denoise = kDenoisers[static_cast<int>(p.denoise)];
    for (const PostEffect e : p.post) r.post.emplace_back(kEffects[static_cast<int>(e)]);
    r.exposure = p.exposure;
    RenderOutcome o = s.render(r);
    if (!o.errors.empty()) {
        reportAll(ctx, o.errors);
        return false;
    }
    out.beginObject();
    out.key("path"); out.string(p.path);
    out.key("frames"); out.int64(o.frames);
    out.key("ms"); out.float32(static_cast<float>(o.ms));
    if (o.validationMessages) { out.key("validation"); out.uint64(*o.validationMessages); }
    out.endObject();
    return true;
}

bool logRead(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    LogReadParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    std::optional<log::Channel> channel;
    if (p.channel) channel = static_cast<log::Channel>(*p.channel);
    const log::Page page = log::read(static_cast<uint64_t>(p.since.value_or(0)), static_cast<log::Level>(p.level),
                                     channel, static_cast<std::size_t>(p.limit));
    out.beginObject();
    out.key("lines");
    out.beginArray();
    for (const log::Record& r : page.lines) {
        out.beginObject();
        out.key("seq"); out.uint64(r.seq);
        out.key("t"); out.float32(static_cast<float>(r.t));
        out.key("level"); out.string(log::levelName(r.level));
        out.key("channel"); out.string(log::channelName(r.channel));
        out.key("msg"); out.string(r.msg);
        if (r.repeat > 1) { out.key("repeat"); out.uint64(r.repeat); }
        out.endObject();
    }
    out.endArray();
    out.key("next"); out.uint64(page.next);
    if (page.dropped) { out.key("dropped"); out.uint64(page.dropped); }
    out.endObject();
    return true;
}

struct MethodEntry {
    std::string_view name;
    MethodFn fn;
};
constexpr MethodEntry kDispatch[] = {
    {"engine.info", engineInfo},     {"schema.get", schemaGet},         {"scene.validate", sceneValidate},
    {"scene.load", sceneLoad},       {"scene.save", sceneSave},         {"world.info", worldInfo},
    {"world.summary", methodSummary}, {"world.query", methodQuery},     {"world.apply", methodApply},
    {"world.step", worldStep},       {"world.snapshot", worldSnapshot}, {"world.restore", worldRestore},
    {"render.capture", renderCapture}, {"log.read", logRead},
};

const MethodEntry* findMethod(std::string_view name) {
    const auto it = std::find_if(std::begin(kDispatch), std::end(kDispatch),
                                 [&](const MethodEntry& e) { return e.name == name; });
    return it == std::end(kDispatch) ? nullptr : it;
}

Diagnostic unknownMethod(std::string_view method) {
    std::vector<std::string_view> names;
    for (const auto& e : kDispatch) names.push_back(e.name);
    Diagnostic d;
    d.code = "E_METHOD";
    d.message = "no method is named '" + std::string(method) + "'";
    d.expected = reflect::oneOf(names);
    d.got = "\"" + std::string(method) + "\"";
    d.suggestion = reflect::suggest(method, names);
    return d;
}

void writeErrorObject(JsonWriter& w, std::vector<Diagnostic>& errors) {
    if (errors.empty()) {
        Diagnostic d;
        d.code = "E_COMMAND";
        d.message = "the request failed without a diagnostic";
        errors.push_back(std::move(d));
    }
    w.beginObject();
    w.key("code"); w.string(errors.front().code);
    w.key("message");
    w.string(errors.size() > 1 ? errors.front().message + " (+" + std::to_string(errors.size() - 1) + " more)"
                               : errors.front().message);
    w.key("data");
    writeDiagnostics(w, errors, kMaxErrorData);
    w.endObject();
}

std::string errorEnvelope(std::vector<Diagnostic>& errors) {
    JsonWriter w(256);
    w.beginObject();
    w.key("error");
    writeErrorObject(w, errors);
    w.endObject();
    return w.take();
}

} // namespace

std::span<const reflect::MethodDoc> methods() {
    static const std::vector<reflect::MethodDoc> table = buildMethods();
    return table;
}

int exitClass(std::string_view code) noexcept {
    if (code == "E_ASSET_NOT_FOUND" || code == "E_ASSET_INVALID" || code == "E_MESH_INVALID") return 2;
    if (code == "E_RENDER" || code == "E_INTERNAL") return 3;
    return 1;
}

std::string_view hostKindName(HostKind k) noexcept {
    switch (k) {
    case HostKind::Cli: return "cli";
    case HostKind::Headless: return "headless";
    case HostKind::Window: return "window";
    case HostKind::None: break;
    }
    return "none";
}

std::string errorObject(std::vector<Diagnostic> errors) {
    JsonWriter w(256);
    writeErrorObject(w, errors);
    return w.take();
}

// ---- helpers --------------------------------------------------------------------------

std::string hex16(uint64_t id) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, id >>= 4) s[static_cast<std::size_t>(i)] = kDigits[id & 15u];
    return s;
}

bool usePath(Session::Impl& s, const JsonNode& params, std::string_view key, std::string_view path, PathUse use,
             DecodeContext& ctx, std::string& resolved) {
    if (!s.paths) {
        resolved = std::string(path);
        return true;
    }
    std::string why;
    const auto p = resolvePath(*s.paths, path, use, why);
    if (p.empty()) {
        ctx.push(key);
        ctx.error("E_ACCESS", *ctx.doc.member(params, key), why,
                  use == PathUse::Read ? "a project-relative path with '/', inside the root"
                                       : "a project-relative path with '/', inside the output directory",
                  "\"" + std::string(path) + "\"");
        ctx.pop();
        return false;
    }
    resolved = p.string();
    return true;
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
    schemaHash = reflect::schemaHash(t, methods());
}

Session::Session(ecs::World& world, const reflect::TypeRegistry& types)
    : m(std::make_unique<Impl>(world, types)) {}
Session::~Session() = default;

ecs::World& Session::world() noexcept { return m->world; }
uint64_t Session::frame() const noexcept { return m->frame; }
uint64_t Session::schemaHash() const noexcept { return m->schemaHash; }
void Session::setAssetResolver(ecs::AssetResolver* assets) noexcept { m->assets = assets; }
void Session::setRenderHandler(RenderHandler handler) { m->render = std::move(handler); }
void Session::setHostKind(HostKind host) noexcept { m->host = host; }
void Session::setPathPolicy(PathPolicy policy) { m->paths = normalizePolicy(std::move(policy)); }

namespace {

// Runs one method, writing its result value into `out`. The status says how it ended;
// errors are left in ctx.
CallStatus run(Session::Impl& m, const MethodEntry& method, const JsonDocument& doc, const JsonNode& params,
               DecodeContext& ctx, JsonWriter& out, const CallControl* control) {
    if (params.type != JsonType::Object) {
        ctx.error("E_TYPE", params, "params is an object", "object", ctx.gotText(params));
        return CallStatus::BadParams;
    }
    m.control = control;
    m.paramsFailed = false;
    m.cancelled = false;
    ecs::WorldEntityIds ids(m.world);
    out.entityIds = &ids;
    const bool ok = method.fn(m, doc, params, ctx, out) && ctx.errors.empty();
    out.entityIds = nullptr;
    m.control = nullptr;
    if (m.cancelled) return CallStatus::Cancelled;
    if (!ok) return m.paramsFailed ? CallStatus::BadParams : CallStatus::Failed;
    return CallStatus::Ok;
}

} // namespace

CallOutcome Session::call(std::string_view method, const JsonDocument& doc, const JsonNode& params,
                          const CallControl* control) {
    CallOutcome o;
    const MethodEntry* found = findMethod(method);
    if (!found) {
        o.status = CallStatus::UnknownMethod;
        o.errors.push_back(unknownMethod(method));
        return o;
    }
    DecodeContext ctx(doc);
    JsonWriter out(256);
    o.status = run(*m, *found, doc, params, ctx, out, control);
    if (o.status == CallStatus::Ok) o.result = out.take();
    else if (o.status != CallStatus::Cancelled) {
        o.errors = std::move(ctx.errors);
        if (o.errors.empty()) {
            Diagnostic d;
            d.code = "E_COMMAND";
            d.message = "the request failed without a diagnostic";
            o.errors.push_back(std::move(d));
        }
    }
    return o;
}

std::string Session::call(std::string_view method, std::string_view params) {
    const MethodEntry* found = findMethod(method);
    if (!found) {
        std::vector<Diagnostic> errs{unknownMethod(method)};
        return errorEnvelope(errs);
    }
    auto parsed = m->paramsParser.parse(params.empty() ? std::string_view("{}") : params);
    if (!parsed) {
        std::vector<Diagnostic> errs{std::move(parsed.error())};
        return errorEnvelope(errs);
    }
    const JsonDocument& doc = *parsed.value();
    DecodeContext ctx(doc);
    // The result is written inside the envelope directly; on failure it is discarded.
    JsonWriter out(256);
    out.beginObject();
    out.key("result");
    if (run(*m, *found, doc, doc.root(), ctx, out, nullptr) != CallStatus::Ok) return errorEnvelope(ctx.errors);
    out.endObject();
    return out.take();
}

} // namespace ohao::api
