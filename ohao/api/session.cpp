#include "api/internal.hpp"

#include "core/log.hpp"
#include "ecs/components.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/world_hash.hpp"
#include "reflect/schema.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace ohao::api {

namespace {

constexpr std::size_t kMaxErrorData = 20;   // diagnostics per error envelope

// ---- the method table -------------------------------------------------------------------

std::vector<reflect::MethodDoc> buildMethods() {
    const reflect::TypeRegistry& p = paramsRegistry();
    return {
        {"engine.info", p.find<EngineInfoParams>(),
         "{protocolVersion, schemaHash, version, host, time, gpu?: {name, driver, rayTracing, meshShaders}}",
         "Handshake: protocol version, schemaHash (key cached catalogues on it), engine version, host, time state; "
         "gpu once the host has a device (headless: at its first render)."},
        {"schema.get", p.find<SchemaGetParams>(), "the catalogue (ohao_schema.json), compact",
         "Every component, field, enum, limit, unit, error code and method. Read once; key on schemaHash."},
        {"scene.validate", p.find<SceneValidateParams>(), "{ok, errors?, warnings?, upgraded?, more?}",
         "Checks a scene exactly as scene.load would, without touching the world. checkAssets also reports model "
         "files that do not exist (E_ASSET_NOT_FOUND)."},
        {"scene.load", p.find<SceneLoadParams>(), "{entities, preserved?, upgraded?, warnings?}",
         "Loads a scene into the world, which must be empty unless replace (then cleared first; snapshots kept). "
         "All or nothing. seed fixes the id generator for later creates."},
        {"scene.save", p.find<SceneSaveParams>(), "{scene} or {path, bytes}",
         "The canonical scene. Without a path it is returned as an object (compact)."},
        {"world.info", p.find<WorldInfoParams>(), "{entities, frame, schemaHash, idCounter, host, time}",
         "Cheap orientation: sizes, identity and the time state."},
        {"world.summary", p.find<WorldSummaryParams>(), "{entities, roots, components, bounds?, outline, moreRoots?}",
         "Counts per component, bounds of entity origins, a capped hierarchy outline (more, moreRoots: children, "
         "roots left out)."},
        {"world.query", p.find<WorldQueryParams>(), "{count, entities, missing?, next?}",
         "Entities in creation order, filtered (all filters must match) and projected. fields: \"/Type\" or "
         "\"/Type/field\"; without fields each entity is its file form."},
        {"world.apply", p.find<WorldApplyParams>(), "{applied, results, inverse}",
         "One atomic batch of commands, applied in order; on any error nothing changes. inverse undoes the batch."},
        {"world.step", p.find<WorldStepParams>(), "{frame, watch?: {entities, count, missing?, next?}}",
         "Runs frames fixed steps (simulate systems per step, then sync once). watch returns fields after the last "
         "step, as world.query. E_STATE while realtime runs. Cancellable between steps."},
        {"time.get", p.find<TimeGetParams>(), "{mode, paused, frame, fixedDt, fps?}",
         "Lockstep (steps on request) or realtime (from wall time), paused, frame (steps run) and the fixed step."},
        {"time.set", p.find<TimeSetParams>(), "as time.get",
         "Changes the mode, pause state or fixed step. Never runs or drops a step. Headless hosts: lockstep only "
         "(E_STATE)."},
        {"world.hash", p.find<WorldHashParams>(), "{hash, frame}",
         "A 64-bit hash of the authoring state (ids, hierarchy, saved components, id generator, creation order). "
         "Runtime state is not hashed."},
        {"world.diff", p.find<WorldDiffParams>(),
         "{changed: [{id, name?, fields: {path: [old, new]}}], created, destroyed, count, more?}",
         "Changes since a snapshot: per entity, \"/Type/field\" (\"/Type\" when added or removed) with old and new "
         "values; created and destroyed ids; more: past limit."},
        {"input.inject", p.find<InputInjectParams>(), "{queued, firstStep, lastStep}",
         "Queues input events by simulation step (0: the next). Actions and axes act as one more device (an axis "
         "holds until changed; null releases it); keys go through the bindings; mouse entries are deltas."},
        {"input.state", p.find<InputStateParams>(), "{actions: {name: {held, pressed, released}}, axes: {name: value}}",
         "The input map as the last step saw it."},
        {"input.record", p.find<InputRecordParams>(), "{path, steps, events}",
         "Starts ({path}) or stops ({stop: true}) a JSON-Lines recording in the output directory: input events by "
         "step, {step, hash} every 60 steps, the final hash. Starting releases held input."},
        {"input.replay", p.find<InputReplayParams>(), "{steps, events, checkpoints, frame}",
         "Replays a recording in lockstep from the current world, which must hash as its start (load with its "
         "seed, or restore). E_REPLAY names the first diverging step. Cancellable."},
        {"world.snapshot", p.find<WorldSnapshotParams>(), "{name, entities, bytes}",
         "Captures the world in memory under a name (replacing it): ids, hierarchy, saved components, id generator, "
         "frame."},
        {"world.restore", p.find<WorldRestoreParams>(), "{created, destroyed, changed, frame}",
         "Brings the world back to a snapshot, writing only what differs. The snapshot is kept for further restores."},
        {"render.capture", p.find<RenderCaptureParams>(),
         "{path?, width, height, imageSize?, frames, ms, readbackMs, encodeMs, gpuMs?, timings?: [[name, ms], ...], "
         "deterministic, validation?} (+ the inline image)",
         "Renders through the Camera to a PNG (path) and/or an inline preview (maxSize; region zooms). A window "
         "captures at its own size; realtime: its next presented frame (deterministic false). Left-out quality, "
         "post, ev100, tonemap: render.settings'. Cancellable, with progress."},
        {"render.settings", p.find<RenderSettingsParams>(), "the RenderSettings now in effect",
         "Reads, or with set changes, the renderer's settings (members left out keep theirs). Host state, never "
         "saved; captures default to them. E_STATE on a host that renders nothing."},
        {"render.stats", p.find<RenderStatsParams>(), "{frame, frames, gpuMs, cpuMs, passes?: [[name, ms], ...], lightsOverflow?, ev100?, skinning?, caustics?, vfx?, latency?, pacing?, audio?}",
         "Medians over the newest frames of the host's GPU time, per pass in frame order. A realtime window adds "
         "latency (look to present) and pacing; a window with sound, audio. E_STATE before a frame."},
        {"log.read", p.find<LogReadParams>(), "{lines: [{seq, t, level, channel, msg, repeat?}], next, dropped?}",
         "Engine log records after since (pass the last read's next), filtered by level and channel."},
        {"level.info", p.find<LevelInfoParams>(),
         "{entity, path, nodes, meshes, uniqueMeshes, triangles, colliders, bodies, spawns: [{tag, entity, pos, yawDeg}], "
         "lights, cameras, warnings, errors?, loadMs}",
         "A Level's summary: counts, colliders by shape, bodies by motion, spawns in node order (first 8). "
         "E_NOT_FOUND for no level or a non-level entity."},
        {"level.reload", p.find<LevelReloadParams>(), "as level.info",
         "Re-reads the level's file and rebuilds its entities and bodies in place; an unchanged file keeps world.hash."},
        {"physics.raycast", p.find<PhysicsRaycastParams>(), "{hit, entity?, name?, point?, normal?, dist?}",
         "The closest collider along a ray. E_PARAMS for a zero dir or max outside (0, 100000]."},
        {"physics.info", p.find<PhysicsInfoParams>(),
         "{bodies, shapes, steps, awake, contacts, triggers?, players?, buildMs, warnings?: [{code, entity, name, message}]}",
         "The physics scene: bodies by motion, shapes by kind, steps run, awake dynamic bodies, pairs in contact."},
        {"physics.events", p.find<PhysicsEventsParams>(),
         "{events: [{seq, step, kind, a, b, an, bn, point, normal, speed, surface?}], next, dropped?}",
         "Contacts begun/ended, triggers entered/exited and players' footsteps and landings, with the ground's surface, "
         "oldest first; pass next as since to read on."},
        {"physics.contacts", p.find<PhysicsContactsParams>(),
         "{contacts: [{a, b, an, bn, trigger?, point, normal, since}], count, more?}",
         "Body pairs touching now (resting included) and bodies in triggers, by id; since: the step they began."},
        {"player.info", p.find<PlayerInfoParams>(),
         "{entity, name, pos, yawDeg, pitchDeg, velocity, ground, on?, surface?, footsteps}",
         "The player: feet, facing, look, velocity, the body it stands on and its surface. Drive it with "
         "input.inject and world.step."},
        {"player.teleport", p.find<PlayerTeleportParams>(), "as player.info",
         "Puts the player's feet at pos, at rest, facing yawDeg (else as it faced); its ground is found there at once."},
        {"combat.info", p.find<CombatInfoParams>(),
         "{steps, round?: {started, phase, restartIn?, restarts}, combatants: [{entity, name, player?, health?, max?, "
         "dead?, pos, weapon?: {ammo, magazine, reloading, bloomDeg, kickDeg, shots}, enemy?: {state, lastSeen?}}], more?}",
         "The fight: the round (playing, dead, won) and everything with Health or a Weapon, in id order. Fire with "
         "input.inject (fire, reload) and world.step."},
        {"combat.events", p.find<CombatEventsParams>(),
         "{events: [{seq, step, kind, source?, sn?, target?, tn?, ...}], next, dropped?}",
         "Shots (point, dir, coneDeg), hits (point, normal, dist, surface), damage (amount, health), deaths, "
         "reloads (ammo), round_start, restart, win; oldest first. Pass next as since to read on."},
        {"combat.damage", p.find<CombatDamageParams>(), "{entity, name, health, max, dead, taken}",
         "Takes amount from the entity's Health (or its nearest ancestor's), as a hit would: damage and death "
         "events; a dead enemy lies down at the next step."},
        {"animation.info", p.find<AnimationInfoParams>(),
         "{steps, animators: [{entity, name, graph, layers: [{state, clip?, t?, from?, blend?}]}], more?, events: "
         "[{seq, step, entity, layer, clip, name}], next, dropped?}",
         "Each Animator's layers (state, clip time in s, the fade) and the clip events after since. Force a state "
         "with component.set on Animator."},
        {"audio.info", p.find<AudioInfoParams>(),
         "{device, sampleRate?, horizonMs, snapshot: {name, blend}, zone, listener?, buses: [{name, db, cap?, voices?, "
         "mix?}], voices: {playing, dropped, stolen, device?}, rays: {cast, capped}, late?, mixMs?, commands: [{seq, "
         "step, op, ...}], next, dropped?}",
         "The voice schedule's commands after since and the bus mix; a window adds the device's half."},
        {"audio.mix", p.find<AudioMixParams>(), "as audio.info, without commands",
         "Listening overrides in the device only (never the world, schedule or snapshots): a bus's db, mute, solo; "
         "a forced snapshot; clear."},
        {"vfx.info", p.find<VfxInfoParams>(),
         "{set, steps, caps, live, dropped?, particles, records: [{seq, step, effect|tracer|decal|light, at, on?}], next}",
         "Effect spawn records the step wrote, after since; particles null headless."},
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
    const Timeline& t = *s.time;
    out.beginObject();
    out.key("mode"); out.string(t.mode() == TimeMode::realtime ? "realtime" : "lockstep");
    out.key("paused"); out.boolean(t.paused());
    out.key("frame"); out.uint64(t.frame());
    out.key("fixedDt"); out.float32(t.fixedDt());
    if (t.fps()) { out.key("fps"); out.float32(*t.fps()); }
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
    if (s.render)
        if (const auto gpu = s.render->gpuInfo()) {
            out.key("gpu");
            out.beginObject();
            out.key("name"); out.string(gpu->name);
            out.key("driver"); out.string(gpu->driver);
            out.key("rayTracing"); out.boolean(gpu->rayTracing);
            out.key("meshShaders"); out.boolean(gpu->meshShaders);
            out.endObject();
        }
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
        s.time->setFrame(0);
        dropInjected(s);
    }
    auto r = ecs::loadScene(s.world, text, s.types);
    if (!r) {
        reportAll(ctx, r.error());
        return false;
    }
    if (p.seed) s.world.seedIds(*p.seed);
    // A host with the level system expands the scene's levels now, so the next query sees
    // their nodes, and one with physics puts its characters at their spawns and builds the
    // bodies, so a snapshot taken now holds the whole simulation a step starts from (a restore
    // to it replays exactly: a character standing on a slope stood one float off when the
    // bodies were built only after the snapshot). Nothing else of sync runs (instantiate and the
    // render extract are the next render's).
    s.world.runSync("level");
    s.world.runSync("player_spawn");
    s.world.runSync("transform");
    s.world.runSync("physics");
    s.scenePath = p.path.value_or("");
    s.sceneSeed = p.seed;
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
    out.key("frame"); out.uint64(s.time->frame());
    out.key("schemaHash"); out.hex64(s.schemaHash);
    out.key("idCounter"); out.uint64(s.world.idGenState().counter);
    out.key("host"); out.string(hostKindName(s.host));
    out.key("time"); writeTime(s, out);
    out.endObject();
    return true;
}

bool snapshotName(Session::Impl& s, const JsonNode& params, const std::string& name, std::string_view what,
                  DecodeContext& ctx);

// E_STATE: the call needs the clock stopped (lockstep, or realtime paused).
bool requireSteppable(Session::Impl& s, const JsonNode& params, std::string_view what, DecodeContext& ctx) {
    if (!s.time->running()) return true;
    ctx.error("E_STATE", params, std::string(what) + " cannot run while the world steps in realtime",
              "time.set {\"paused\": true} or {\"mode\": \"lockstep\"} first");
    return false;
}

bool worldStep(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    WorldStepParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const float dt = p.dt.value_or(s.time->fixedDt());
    if (!(dt > 0.0f)) {
        ctx.push("dt");
        ctx.error("E_RANGE", *ctx.doc.member(params, "dt"), "dt is out of range", "a step in (0, 1] seconds",
                  ctx.gotText(*ctx.doc.member(params, "dt")));
        ctx.pop();
        return badParams(s);
    }
    // The watch is checked before any step runs: a bad path costs no simulation.
    QueryPlan watch;
    if (p.watch) {
        const JsonNode& w = *ctx.doc.member(params, "watch");
        ctx.push("watch");
        if (!ctx.doc.member(w, "fields"))
            ctx.error("E_COMMAND", w, "a watch needs \"fields\"", "{\"fields\": [\"/Transform/position\"]}");
        watch.limit = static_cast<std::size_t>(p.watch->limit);
        planQuery(s, ctx.doc, w, ctx, watch);
        ctx.pop();
        if (!ctx.errors.empty()) return badParams(s);
    }
    if (!requireSteppable(s, params, "world.step", ctx)) return false;
    const auto frames = static_cast<uint64_t>(p.frames);
    for (uint64_t i = 0; i < frames; ++i) {
        if (s.cancelRequested()) {   // between whole steps
            s.world.sync();
            s.cancelled = true;
            return false;
        }
        stepOnce(s, {}, dt);
        if (s.control && s.control->progress) s.control->progress(i + 1, frames);
    }
    s.world.sync();   // derived state once per batch
    out.beginObject();
    out.key("frame"); out.uint64(s.time->frame());
    if (p.watch) {
        out.key("watch");
        writeQuery(s, watch, out);
    }
    out.endObject();
    return true;
}

bool timeGet(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    TimeGetParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    writeTime(s, out);
    return true;
}

bool timeSet(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    TimeSetParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (p.fixedDt && !(*p.fixedDt > 0.0f)) {
        ctx.push("fixedDt");
        ctx.error("E_RANGE", *doc.member(params, "fixedDt"), "fixedDt is out of range", "a step in (0, 1] seconds",
                  ctx.gotText(*doc.member(params, "fixedDt")));
        ctx.pop();
        return badParams(s);
    }
    // Realtime needs a clock that runs between requests: only a window host has one.
    if (p.mode == TimeMode::realtime && s.host != HostKind::Window) {
        ctx.push("mode");
        ctx.error("E_STATE", *doc.member(params, "mode"),
                  "this host runs lockstep only (" + std::string(hostKindName(s.host)) + ")",
                  "\"lockstep\"; realtime needs a window host", "\"realtime\"");
        ctx.pop();
        return false;
    }
    if (p.mode) s.time->setMode(*p.mode);
    if (p.paused) s.time->setPaused(*p.paused);
    if (p.fixedDt)
        s.time->setFixedStep(Nanos(static_cast<int64_t>(std::llround(static_cast<double>(*p.fixedDt) * 1e9))));
    writeTime(s, out);
    return true;
}

bool worldHash(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    WorldHashParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const uint64_t h = ecs::hashWorld(s.world, s.types, s.hashScratch);
    out.beginObject();
    out.key("hash"); out.hex64(h);
    out.key("frame"); out.uint64(s.time->frame());
    out.endObject();
    return true;
}

// The snapshot a request names, or E_SNAPSHOT_UNKNOWN (with the known names) at `key`.
const ecs::Snapshot* findSnapshot(Session::Impl& s, const JsonDocument& doc, const JsonNode& params,
                                  std::string_view key, const std::string& name, DecodeContext& ctx) {
    const auto it = s.snapshots.find(name);
    if (it != s.snapshots.end()) return &it->second;
    std::vector<std::string_view> names;
    for (const auto& [n, snap] : s.snapshots) names.push_back(n);
    ctx.push(key);
    ctx.error("E_SNAPSHOT_UNKNOWN", *doc.member(params, key), "no snapshot is named '" + name + "'",
              names.empty() ? "a name given to world.snapshot (none yet)" : reflect::oneOf(names), "\"" + name + "\"",
              reflect::suggest(name, names));
    ctx.pop();
    return nullptr;
}

bool worldDiff(Session::Impl& s, const JsonDocument& doc, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    WorldDiffParams p;
    if (!decodeParams(s, params, ctx, p) || !snapshotName(s, params, p.snapshot, "world.diff", ctx)) return false;
    std::vector<Projection> only;
    if (p.fields) {
        parseProjection(s, doc, *doc.member(params, "fields"), ctx, only);
        if (!ctx.errors.empty()) return badParams(s);
    }
    const ecs::Snapshot* snap = findSnapshot(s, doc, params, "snapshot", p.snapshot, ctx);
    if (!snap) return false;
    ecs::WorldDiff& d = s.diffScratch;
    ecs::diffWorld(s.world, *snap, d);

    const auto projected = [&](const TypeInfo* t) -> const Projection* {
        for (const Projection& pr : only)
            if (pr.type == t) return &pr;
        return nullptr;
    };
    // Whether a component change is listed, and which of its fields. A whole-type projection
    // (or none at all) lists every field.
    const auto fieldListed = [&](const Projection* pr, const FieldInfo& f) {
        return !pr || pr->whole || std::find(pr->fields.begin(), pr->fields.end(), &f) != pr->fields.end();
    };
    const auto anyListed = [&](const ecs::EntityDiff& e) {
        if (only.empty()) return true;
        for (const ecs::ComponentDiff& c : e.components) {
            const Projection* pr = projected(c.type);
            if (!pr) continue;
            if (!c.before || !c.after || pr->whole) return true;
            for (const FieldInfo* f : pr->fields)
                if (!f->ops->equal(c.before, c.after, *f->type)) return true;
        }
        return e.parentChanged && projected(s.tParent);
    };
    const auto parentValue = [&](uint64_t id) {
        out.beginObject();
        out.key("entity"); out.hex64(id);
        out.endObject();
    };

    const auto limit = static_cast<std::size_t>(p.limit);
    std::size_t count = 0, more = 0;
    out.beginObject();
    out.key("changed");
    out.beginArray();
    for (const ecs::EntityDiff& e : d.changed) {
        if (!anyListed(e)) continue;
        if (count++ >= limit) {
            ++more;
            continue;
        }
        out.beginObject();
        out.key("id"); out.hex64(e.id);
        if (const auto* n = s.world.registry().try_get<ecs::Name>(e.entity); n && !n->value.empty()) {
            out.key("name"); out.string(n->value);
        }
        out.key("fields");
        out.beginObject();
        for (const ecs::ComponentDiff& c : e.components) {
            const Projection* pr = projected(c.type);
            if (!only.empty() && !pr) continue;
            if (!c.before || !c.after) {   // added or removed: the whole value, in file form
                out.key("/" + c.type->name);
                out.beginArray();
                if (c.before) reflect::writeStruct(c.before, out, *c.type, c.type->defaultValue.get());
                else out.null();
                if (c.after) reflect::writeStruct(c.after, out, *c.type, c.type->defaultValue.get());
                else out.null();
                out.endArray();
                continue;
            }
            for (const FieldInfo& f : c.type->fields) {
                if (!fieldListed(pr, f) || f.ops->equal(c.before, c.after, *f.type)) continue;
                out.key("/" + c.type->name + "/" + f.name);
                out.beginArray();
                f.ops->write(c.before, out, *f.type);
                f.ops->write(c.after, out, *f.type);
                out.endArray();
            }
        }
        if (e.parentChanged && (only.empty() || projected(s.tParent))) {
            if (e.parentBefore && e.parentAfter) {
                out.key("/Parent/entity");
                out.beginArray();
                out.hex64(e.parentBefore);
                out.hex64(e.parentAfter);
                out.endArray();
            } else {
                out.key("/Parent");
                out.beginArray();
                if (e.parentBefore) parentValue(e.parentBefore);
                else out.null();
                if (e.parentAfter) parentValue(e.parentAfter);
                else out.null();
                out.endArray();
            }
        }
        out.endObject();
        out.endObject();
    }
    out.endArray();
    const auto ids = [&](const char* key, const std::vector<uint64_t>& list) {
        out.key(key);
        out.beginArray();
        for (std::size_t i = 0; i < list.size() && i < limit; ++i) out.hex64(list[i]);
        out.endArray();
        if (list.size() > limit) more += list.size() - limit;
    };
    ids("created", d.created);
    ids("destroyed", d.destroyed);
    out.key("count"); out.uint64(count);
    if (more) { out.key("more"); out.uint64(more); }
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
    ecs::takeSnapshot(s.world, s.types, it->second, s.time->frame());   // reuses the old snapshot's memory
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
    const ecs::Snapshot* snap = findSnapshot(s, doc, params, "name", p.name, ctx);
    if (!snap) return false;
    const ecs::RestoreStats st = ecs::restoreSnapshot(s.world, *snap);
    s.time->setFrame(snap->frame);
    dropInjected(s);
    out.beginObject();
    out.key("created"); out.uint64(st.created);
    out.key("destroyed"); out.uint64(st.destroyed);
    out.key("changed"); out.uint64(st.changed);
    out.key("frame"); out.uint64(s.time->frame());
    out.endObject();
    return true;
}

std::string base64(std::string_view bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    s.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8) |
                           static_cast<uint8_t>(bytes[i + 2]);
        s += kAlphabet[v >> 18];
        s += kAlphabet[(v >> 12) & 63];
        s += kAlphabet[(v >> 6) & 63];
        s += kAlphabet[v & 63];
    }
    if (const std::size_t rest = bytes.size() - i; rest > 0) {
        uint32_t v = static_cast<uint8_t>(bytes[i]) << 16;
        if (rest == 2) v |= static_cast<uint8_t>(bytes[i + 1]) << 8;
        s += kAlphabet[v >> 18];
        s += kAlphabet[(v >> 12) & 63];
        s += rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
        s += '=';
    }
    return s;
}

bool renderCapture(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                   JsonWriter& out) {
    RenderCaptureParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (!p.path && !p.inlineImage && !p.hdr) {
        ctx.error("E_COMMAND", params,
                  "render.capture needs a \"path\" for the PNG, an \"hdr\" for the linear PFM, or \"inline\": true",
                  "{\"path\": \"out.png\"} or {\"inline\": true}");
        return badParams(s);
    }
    if (p.path && p.path->empty()) {
        ctx.push("path");
        ctx.error("E_COMMAND", *ctx.doc.member(params, "path"), "the path is empty", "a file name such as \"out.png\"");
        ctx.pop();
        return badParams(s);
    }
    if (p.hdr && p.hdr->empty()) {
        ctx.push("hdr");
        ctx.error("E_COMMAND", *ctx.doc.member(params, "hdr"), "the path is empty", "a file name such as \"out.pfm\"");
        ctx.pop();
        return badParams(s);
    }
    if (p.region && (p.region->size() != 4 || (*p.region)[2] < 1 || (*p.region)[3] < 1)) {
        ctx.push("region");
        ctx.error("E_COMMAND", *ctx.doc.member(params, "region"), "region is [x, y, w, h] with w and h at least 1",
                  "[0, 0, 640, 360]");
        ctx.pop();
        return badParams(s);
    }
    if (!s.render) {
        ctx.error("E_COMMAND", params, "this session has no renderer; render.capture needs a host with a GPU (ohao run)");
        return false;
    }
    std::string file;
    if (p.path && !usePath(s, params, "path", *p.path, PathUse::Write, ctx, file)) return false;
    std::string hdrFile;
    if (p.hdr && !usePath(s, params, "hdr", *p.hdr, PathUse::Write, ctx, hdrFile)) return false;
    static constexpr const char* kModes[] = {"deferred", "rt_realtime", "rt_offline"};
    static constexpr const char* kDenoisers[] = {"none", "oidn", "nrd", "dlss"};
    static constexpr const char* kEffects[] = {"bloom", "taa"};
    RenderRequest r;
    r.path = file;
    r.hdrPath = hdrFile;
    r.mode = kModes[static_cast<int>(p.mode)];
    r.spp = p.spp;
    if (p.bounces) r.bounces = *p.bounces;
    r.frames = p.frames;
    if (p.width) r.width = static_cast<uint32_t>(*p.width);
    if (p.height) r.height = static_cast<uint32_t>(*p.height);
    r.denoise = kDenoisers[static_cast<int>(p.denoise)];
    // What the capture leaves out comes from the host's render.settings.
    const std::optional<RenderSettings> settings = s.render->renderSettings();
    if (p.post)
        for (const PostEffect e : *p.post) r.post.emplace_back(kEffects[static_cast<int>(e)]);
    r.ev100 = p.ev100 ? p.ev100 : (settings ? std::optional<float>(settings->ev100) : std::nullopt);
    if (const std::optional<Tonemap> t = p.tonemap ? p.tonemap : (settings ? std::optional<Tonemap>(settings->tonemap)
                                                                             : std::nullopt))
        r.tonemap = *t == Tonemap::neutral ? "neutral" : "agx";
    const Quality quality = p.quality ? *p.quality : (settings ? settings->quality : Quality::realtime);
    if (quality == Quality::reference) r.quality = "reference";
    r.timings = p.timings;
    r.hud = p.hud;
    r.guiding = p.guiding;
    r.caustics = p.caustics;
    r.inlineImage = p.inlineImage;
    r.format = p.format == ImageFormat::png ? "png" : "jpeg";
    r.maxSize = p.maxSize;
    if (p.region) r.region = std::array<int32_t, 4>{(*p.region)[0], (*p.region)[1], (*p.region)[2], (*p.region)[3]};
    r.control = s.control;
    RenderOutcome o = s.render->capture(r);
    if (o.cancelled) {   // between frames: no image, no answer (the host is as it was)
        s.cancelled = true;
        return false;
    }
    if (!o.errors.empty()) {
        reportAll(ctx, o.errors);
        return false;
    }
    out.beginObject();
    if (p.path) { out.key("path"); out.string(*p.path); }
    if (p.hdr) { out.key("hdr"); out.string(*p.hdr); }
    out.key("width"); out.uint64(o.width);
    out.key("height"); out.uint64(o.height);
    if (p.inlineImage) {
        out.key("imageSize");
        out.beginArray(); out.uint64(o.imageWidth); out.uint64(o.imageHeight); out.endArray();
    }
    out.key("frames"); out.int64(o.frames);
    out.key("ms"); out.float32(static_cast<float>(o.ms));
    out.key("readbackMs"); out.float32(static_cast<float>(o.readbackMs));
    out.key("encodeMs"); out.float32(static_cast<float>(o.encodeMs));
    const auto ms = [](double v) { return static_cast<float>(std::round(v * 1000.0) / 1000.0); };
    if (o.gpuMs) { out.key("gpuMs"); out.float32(ms(*o.gpuMs)); }
    if (p.timings) {
        out.key("timings");
        out.beginArray();
        for (const auto& [name, t] : o.timings) {
            out.beginArray(); out.string(name); out.float32(ms(t)); out.endArray();
        }
        out.endArray();
    }
    out.key("deterministic"); out.boolean(o.deterministic);
    if (o.validationMessages) { out.key("validation"); out.uint64(*o.validationMessages); }
    out.endObject();
    if (p.inlineImage) s.image = ResultImage{o.imageMime, base64(o.image)};
    return true;
}

bool renderSettings(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                    JsonWriter& out) {
    RenderSettingsParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    std::optional<RenderSettings> current = s.render ? s.render->renderSettings() : std::nullopt;
    if (!current) {
        ctx.error("E_STATE", params, "this host renders nothing of its own: render.settings needs a GPU host",
                  "ohao serve (headless or --window)");
        return false;
    }
    if (const JsonNode* set = ctx.doc.member(params, "set")) {
        // The members given change; the rest keep their value (decoded onto the current ones).
        ctx.push("set");
        reflect::readStruct(&*current, *paramsRegistry().find<RenderSettings>(), *set, ctx);
        ctx.pop();
        if (!ctx.errors.empty()) return badParams(s);
        s.render->setRenderSettings(*current);
        ++s.changes;   // a change, as a mutating method's: a lockstep window redraws
        current = s.render->renderSettings();
    }
    reflect::writeStruct(&*current, out, *paramsRegistry().find<RenderSettings>());
    return true;
}

bool renderStats(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                 JsonWriter& out) {
    RenderStatsParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (!s.render) {
        ctx.error("E_COMMAND", params, "this session has no renderer; render.stats needs a host with a GPU");
        return false;
    }
    const std::optional<RenderStats> st = s.render->renderStats(p.frames);
    if (!st) {
        ctx.error("E_STATE", params, "the host has not finished rendering a frame yet",
                  "render.capture (or a window host's next frame) first");
        return false;
    }
    // Microseconds: "0.071", about 20 bytes a pass.
    const auto ms = [](double v) { return static_cast<float>(std::round(v * 1000.0) / 1000.0); };
    out.beginObject();
    out.key("frame"); out.uint64(st->frame);
    out.key("frames"); out.int64(st->frames);
    out.key("gpuMs"); out.float32(ms(st->gpuMs));
    out.key("cpuMs"); out.float32(ms(st->cpuMs));
    if (p.passes) {
        out.key("passes");
        out.beginArray();
        for (const auto& [name, t] : st->passes) {
            out.beginArray(); out.string(name); out.float32(ms(t)); out.endArray();
        }
        out.endArray();
    }
    if (st->lightsOverflow) { out.key("lightsOverflow"); out.uint64(*st->lightsOverflow); }
    if (st->ev100) { out.key("ev100"); out.float32(*st->ev100); }
    if (const auto& c = st->caustics) {
        out.key("caustics");
        out.beginObject();
        out.key("casters"); out.uint64(c->casters);
        out.key("clamped"); out.uint64(c->clamped);
        out.endObject();
    }
    if (const auto& k = st->skinning) {
        out.key("skinning");
        out.beginObject();
        out.key("instances"); out.uint64(k->instances);
        out.key("skinned"); out.uint64(k->skinned);
        out.key("vertices"); out.uint64(k->vertices);
        out.key("blasRefits"); out.uint64(k->blasRefits);
        out.key("blasRebuilds"); out.uint64(k->blasRebuilds);
        out.endObject();
    }
    if (const auto& l = st->latency) {
        out.key("latency");
        out.beginObject();
        out.key("frames"); out.int64(l->frames);
        out.key("samples"); out.int64(l->samples);
        out.key("medianMs"); out.float32(ms(l->medianMs));
        out.key("p95Ms"); out.float32(ms(l->p95Ms));
        out.key("source"); out.string(l->presentWait ? "present_wait" : "present_call");
        out.key("present"); out.string(l->present);
        out.key("stages");
        out.beginObject();
        out.key("pollToConsumeMs"); out.float32(ms(l->pollToConsumeMs));
        out.key("pollToViewMs"); out.float32(ms(l->pollToViewMs));
        out.key("viewToSubmitMs"); out.float32(ms(l->viewToSubmitMs));
        out.key("submitToPresentMs"); out.float32(ms(l->submitToPresentMs));
        out.endObject();
        out.endObject();
    }
    if (const auto& pc = st->pacing) {
        out.key("pacing");
        out.beginObject();
        out.key("frames"); out.int64(pc->frames);
        out.key("frameMs");
        out.beginObject();
        out.key("median"); out.float32(ms(pc->medianMs));
        out.key("p95"); out.float32(ms(pc->p95Ms));
        out.key("p99"); out.float32(ms(pc->p99Ms));
        out.endObject();
        out.key("steps");
        out.beginObject();
        static constexpr const char* kSteps[] = {"0", "1", "2", "3+"};
        for (std::size_t i = 0; i < 4; ++i) { out.key(kSteps[i]); out.uint64(pc->steps[i]); }
        out.endObject();
        out.key("discardedSteps"); out.uint64(pc->discardedSteps);
        out.key("maxFps"); out.int64(pc->maxFps);
        out.endObject();
    }
    if (const auto& a = st->audio) {
        out.key("audio");
        out.beginObject();
        out.key("mixMs"); out.float32(ms(a->mixMs));
        out.key("voices"); out.uint64(a->voices);
        out.key("late"); out.uint64(a->late);
        out.endObject();
    }
    if (const auto& v = st->vfx) {
        out.key("vfx");
        out.beginObject();
        out.key("particles"); out.uint64(v->particles);
        out.key("objectsSorted"); out.uint64(v->objectsSorted);
        out.key("decalsDrawn"); out.uint64(v->decalsDrawn);
        out.key("tracers"); out.uint64(v->tracers);
        out.key("proxies"); out.uint64(v->proxies);
        out.key("particlesSortedMax"); out.uint64(v->particlesSortedMax);
        out.endObject();
    }
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
    bool mutates{false};   // may change the world (Session::changeCount)
};
constexpr MethodEntry kDispatch[] = {
    {"engine.info", engineInfo},     {"schema.get", schemaGet},         {"scene.validate", sceneValidate},
    {"scene.load", sceneLoad, true}, {"scene.save", sceneSave},         {"world.info", worldInfo},
    {"world.summary", methodSummary}, {"world.query", methodQuery},     {"world.apply", methodApply, true},
    {"world.step", worldStep, true}, {"time.get", timeGet},             {"time.set", timeSet},
    {"world.hash", worldHash},       {"world.diff", worldDiff},         {"input.inject", methodInputInject},
    {"input.state", methodInputState}, {"input.record", methodInputRecord},
    {"input.replay", methodInputReplay, true},
    {"world.snapshot", worldSnapshot},
    {"world.restore", worldRestore, true}, {"render.capture", renderCapture},
    {"render.stats", renderStats},   {"render.settings", renderSettings}, {"log.read", logRead},
    {"level.info", methodLevelInfo}, {"level.reload", methodLevelReload, true},
    {"physics.raycast", methodPhysicsRaycast}, {"physics.info", methodPhysicsInfo}, {"physics.events", methodPhysicsEvents},
    {"physics.contacts", methodPhysicsContacts}, {"player.info", methodPlayerInfo},
    {"player.teleport", methodPlayerTeleport, true},
    {"combat.info", methodCombatInfo}, {"combat.events", methodCombatEvents},
    {"combat.damage", methodCombatDamage, true},
    {"animation.info", methodAnimationInfo},
    {"audio.info", methodAudioInfo},  {"audio.mix", methodAudioMix},
    {"vfx.info", methodVfxInfo},
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
    if (code == "E_ASSET_NOT_FOUND" || code == "E_ASSET_INVALID" || code == "E_MESH_INVALID" ||
        code.starts_with("E_LEVEL_"))
        return 2;
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
    : m(std::make_unique<Impl>(world, types)) {
    // A Round in the world keeps its start as a snapshot with these types (the combat system's
    // RoundHook; ECS system code has no reflection of its own).
    ecs::installRoundHook(world, types);
}
Session::~Session() = default;

ecs::World& Session::world() noexcept { return m->world; }
uint64_t Session::frame() const noexcept { return m->time->frame(); }
Timeline& Session::timeline() noexcept { return *m->time; }
void Session::setTimeline(Timeline& timeline) noexcept { m->time = &timeline; }
void Session::step(std::span<const InputEvent> deviceEvents, float dt) {
    ++m->changes;
    stepOnce(*m, deviceEvents, dt);
}
std::span<const std::chrono::steady_clock::time_point> Session::lastStepInjectedLook() const noexcept {
    return m->lastStepInjectedLook;
}
void Session::injectedLookDue(glm::vec2& sum, std::vector<std::chrono::steady_clock::time_point>& queuedAt) const {
    const uint64_t next = m->time->frame();
    for (const auto& p : m->injected) {
        if (p.step > next) break;   // in apply order: the rest are for later steps
        if (p.event.type != InputEvent::Type::MouseDelta) continue;
        sum += glm::vec2(static_cast<float>(p.event.x), static_cast<float>(p.event.y));   // as InputMap adds it
        queuedAt.push_back(p.queuedAt);
    }
}
uint64_t Session::schemaHash() const noexcept { return m->schemaHash; }
void Session::setAssetResolver(ecs::AssetResolver* assets) noexcept { m->assets = assets; }
void Session::setHost(Host* host) noexcept {
    m->render = host;
    if (host) m->host = host->kind();
}
void Session::setHostKind(HostKind host) noexcept { m->host = host; }
uint64_t Session::changeCount() const noexcept { return m->changes; }
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
    m.image.reset();
    if (method.mutates) ++m.changes;   // counted whatever the outcome: a cancelled step moved the world too
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
    if (o.status == CallStatus::Ok) {
        o.result = out.take();
        o.image = std::exchange(m->image, std::nullopt);
    } else if (o.status != CallStatus::Cancelled) {
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
    if (!m->image) {
        out.endObject();
        return out.take();
    }
    const std::string result = out.take();   // {"result":{...}
    const ResultImage image = *std::exchange(m->image, std::nullopt);
    return withImage(result, image) + "}";
}

std::string withImage(std::string_view result, const ResultImage& image) {
    std::string s(result.substr(0, result.size() - 1));   // the object without its closing brace
    s.reserve(s.size() + image.base64.size() + 64);
    if (!s.empty() && s.back() != '{') s += ',';
    s += "\"image\":\"";
    s += image.base64;   // base64 needs no escaping
    s += "\",\"mimeType\":\"";
    s += image.mimeType;
    s += "\"}";
    return s;
}

} // namespace ohao::api
