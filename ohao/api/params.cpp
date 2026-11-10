#include "api/params.hpp"

#include "ecs/scene_types.hpp"   // registerKeyEnum

#include <limits>

namespace ohao::api {

namespace {

void registerParams(reflect::TypeRegistry& r) {
    r.enumeration<SaveLayout>("SaveLayout").doc("Scene file layout")
        .value("compact", SaveLayout::compact, "No whitespace")
        .value("pretty", SaveLayout::pretty, "The canonical file layout");
    r.enumeration<CaptureMode>("CaptureMode").doc("Render mode of a capture")
        .value("deferred", CaptureMode::deferred, "Deferred raster with RT shadows and GI")
        .value("rt_realtime", CaptureMode::rt_realtime, "Real-time path tracer")
        .value("rt_offline", CaptureMode::rt_offline, "Offline path tracer");
    r.enumeration<Denoiser>("Denoiser").doc("Path tracer denoiser")
        .value("none", Denoiser::none, "No denoiser")
        .value("oidn", Denoiser::oidn, "Intel Open Image Denoise")
        .value("nrd", Denoiser::nrd, "NVIDIA Real-time Denoisers")
        .value("dlss", Denoiser::dlss, "DLSS Ray Reconstruction");
    r.enumeration<Tonemap>("Tonemap").doc("The display curve")
        .value("agx", Tonemap::agx, "AgX (Sobotka): neutral filmic, little hue shift")
        .value("neutral", Tonemap::neutral, "Khronos PBR Neutral: base colours shown as themselves");
    r.enumeration<PostEffect>("PostEffect").doc("Deferred post effect")
        .value("bloom", PostEffect::bloom, "Bloom")
        .value("taa", PostEffect::taa, "Temporal anti-aliasing");
    r.enumeration<Quality>("Quality").doc("Hybrid deferred frame quality")
        .value("realtime", Quality::realtime, "The budgeted frame: RT shadows from up to 4 lights a pixel, 1 ray each, filtered")
        .value("reference", Quality::reference,
               "Its converged answer: every shadowed light, 8 rays a frame, averaged over the capture's frames");
    r.enumeration<LogLevelName>("LogLevel").doc("Log level")
        .value("trace", LogLevelName::trace, "Everything")
        .value("debug", LogLevelName::debug, "Per-item detail")
        .value("info", LogLevelName::info, "Lifecycle")
        .value("warn", LogLevelName::warn, "Degradations")
        .value("error", LogLevelName::error, "Failures");
    r.enumeration<LogChannelName>("LogChannel").doc("Log channel")
        .value("render", LogChannelName::render, "Renderer")
        .value("gpu", LogChannelName::gpu, "Vulkan device and resources")
        .value("validation", LogChannelName::validation, "Vulkan validation messages")
        .value("ecs", LogChannelName::ecs, "World and systems")
        .value("asset", LogChannelName::asset, "Asset loading")
        .value("physics", LogChannelName::physics, "Physics")
        .value("audio", LogChannelName::audio, "Audio")
        .value("api", LogChannelName::api, "API methods")
        .value("rpc", LogChannelName::rpc, "Protocol server")
        .value("runtime", LogChannelName::runtime, "Window and main loop")
        .value("vfx", LogChannelName::vfx, "Effects");

    r.type<SchemaGetParams>("SchemaGetParams").doc("schema.get params");
    r.type<EngineInfoParams>("EngineInfoParams").doc("engine.info params");
    r.type<WorldInfoParams>("WorldInfoParams").doc("world.info params");

    r.type<SceneValidateParams>("SceneValidateParams").doc("scene.validate params")
        .field("text", &SceneValidateParams::text).doc("Scene file text (give one of text, path, scene)")
        .field("path", &SceneValidateParams::path).doc("Scene file path, project-relative with '/'")
        .field("scene", &SceneValidateParams::scene).doc("Scene document as a JSON object")
        .field("limit", &SceneValidateParams::limit).doc("Errors and warnings listed, each").unitless().min(0).max(100)
        .field("checkAssets", &SceneValidateParams::checkAssets).doc("Also report model files that do not exist");

    r.type<SceneLoadParams>("SceneLoadParams").doc("scene.load params")
        .field("text", &SceneLoadParams::text).doc("Scene file text (give one of text, path, scene)")
        .field("path", &SceneLoadParams::path).doc("Scene file path, project-relative with '/'")
        .field("scene", &SceneLoadParams::scene).doc("Scene document as a JSON object")
        .field("seed", &SceneLoadParams::seed).doc("Id generator seed, 16 hex digits: fixes the ids later creates get")
        .field("replace", &SceneLoadParams::replace).doc("Clear the world first (snapshots are kept); else it must be empty");

    r.type<SceneSaveParams>("SceneSaveParams").doc("scene.save params")
        .field("path", &SceneSaveParams::path).doc("File to write, under the output directory; absent: returned inline")
        .field("layout", &SceneSaveParams::layout).doc("Default: pretty for a file, compact inline")
        .field("defaults", &SceneSaveParams::defaults).doc("Write fields equal to their defaults too");

    r.type<WorldSummaryParams>("WorldSummaryParams").doc("world.summary params")
        .field("depth", &WorldSummaryParams::depth).doc("Hierarchy levels in the outline").unitless().min(0).max(64)
        .field("maxChildren", &WorldSummaryParams::maxChildren).doc("Children listed per node").unitless().min(1).max(10'000)
        .field("maxNodes", &WorldSummaryParams::maxNodes).doc("Nodes in the whole outline").unitless().min(1).max(100'000);

    r.type<WorldQueryParams>("WorldQueryParams").doc("world.query params")
        .field("ids", &WorldQueryParams::ids).doc("Entity ids (16 hex digits)")
        .field("names", &WorldQueryParams::names).doc("Exact names")
        .field("nameContains", &WorldQueryParams::nameContains).doc("Substring of the name")
        .field("has", &WorldQueryParams::has).doc("Component names the entity must all have")
        .field("fields", &WorldQueryParams::fields).doc("Projection: \"/Type\" or \"/Type/field\"; absent: whole entities")
        .field("limit", &WorldQueryParams::limit).doc("Entities returned").unitless().min(0).max(100'000)
        .field("offset", &WorldQueryParams::offset).doc("Matching entities skipped first (paging)").unitless().min(0)
            .max(std::numeric_limits<int32_t>::max())
        .field("defaults", &WorldQueryParams::defaults).doc("Include fields equal to their defaults");

    r.type<WorldApplyParams>("WorldApplyParams").doc("world.apply params")
        .field("commands", &WorldApplyParams::commands).doc("Array of commands {op, ...}, applied atomically in order")
            .required().maxItems(kMaxApplyCommands);

    r.type<WatchSpec>("WatchSpec").doc("Entities and fields world.step returns after its last step")
        .field("ids", &WatchSpec::ids).doc("Entity ids (16 hex digits)")
        .field("names", &WatchSpec::names).doc("Exact names")
        .field("fields", &WatchSpec::fields).doc("Projection: \"/Type\" or \"/Type/field\"").required()
        .field("limit", &WatchSpec::limit).doc("Entities returned").unitless().min(0).max(10'000);

    r.type<WorldStepParams>("WorldStepParams").doc("world.step params")
        .field("frames", &WorldStepParams::frames).doc("Fixed steps to run").unitless().min(0).max(1'000'000)
        .field("dt", &WorldStepParams::dt).doc("Step length, in (0, 1]; default the session's fixedDt (time.get)")
            .unit("s").min(0.0).max(1.0)
        .field("watch", &WorldStepParams::watch).doc("Fields to return after the last step: one round trip, not two");

    r.enumeration<TimeMode>("TimeMode").doc("How the world advances")
        .value("lockstep", TimeMode::lockstep, "Only on request (world.step); the default for ohao serve")
        .value("realtime", TimeMode::realtime, "Fixed steps from wall time, as a game runs (window hosts only)");
    r.type<TimeGetParams>("TimeGetParams").doc("time.get params");
    r.type<TimeSetParams>("TimeSetParams").doc("time.set params")
        .field("mode", &TimeSetParams::mode).doc("lockstep or realtime (realtime needs a window host)")
        .field("paused", &TimeSetParams::paused).doc("Stop realtime stepping; world.step then works as in lockstep")
        .field("fixedDt", &TimeSetParams::fixedDt).doc("Fixed step length, in (0, 1]").unit("s").min(0.0).max(1.0);

    r.type<WorldHashParams>("WorldHashParams").doc("world.hash params");

    ecs::registerKeyEnum(r);
    r.type<InjectEvent>("InjectEvent").doc("One input event; exactly one of action, axis, key, mouse, focusLost")
        .field("step", &InjectEvent::step).doc("Steps after the next step to run (0: the next); not in the past")
            .unitless().min(0).max(10'000'000)
        .field("action", &InjectEvent::action).doc("An action held (down: true) or let go (down: false)")
        .field("axis", &InjectEvent::axis).doc("An axis forced to value until changed; value null releases it")
        .field("key", &InjectEvent::key).doc("A key pressed or released (down), through the bindings like a real key")
        .field("mouse", &InjectEvent::mouse).doc("A mouse movement [dx, dy]").unitless()
        .field("down", &InjectEvent::down).doc("For action and key: held (true) or let go (false)")
        .field("value", &InjectEvent::value).doc("For axis: the value, or null to release it").unitless()
        .field("focusLost", &InjectEvent::focusLost).doc("Releases everything held, real and injected");
    r.type<InputInjectParams>("InputInjectParams").doc("input.inject params")
        .field("events", &InputInjectParams::events).doc("Events in order; same-step events apply in this order, "
                                                          "after that step's device events").required()
            .maxItems(kMaxInjectEvents)
        .field("player", &InputInjectParams::player)
            .doc("The player (16 hex digits) whose input stream (PlayerController.stream) takes the events; absent: "
                 "stream 0, a device's");
    r.type<InputStateParams>("InputStateParams").doc("input.state params")
        .field("player", &InputStateParams::player)
            .doc("The player (16 hex digits) whose input stream to read; absent: stream 0, a device's");
    r.type<InputRecordParams>("InputRecordParams").doc("input.record params")
        .field("path", &InputRecordParams::path).doc("JSON-Lines file to write, under the output directory (starts)")
        .field("stop", &InputRecordParams::stop).doc("Stops the recording in progress");
    r.type<InputReplayParams>("InputReplayParams").doc("input.replay params")
        .field("path", &InputReplayParams::path).doc("A recording written by input.record (or by hand)").required()
        .field("verify", &InputReplayParams::verify).doc("Require the start hash and compare every checkpoint");
    r.type<WorldDiffParams>("WorldDiffParams").doc("world.diff params")
        .field("snapshot", &WorldDiffParams::snapshot).doc("Name given to world.snapshot").required()
        .field("fields", &WorldDiffParams::fields).doc("Only these: \"/Type\" or \"/Type/field\"; absent: everything")
        .field("limit", &WorldDiffParams::limit).doc("Entries listed in each of changed, created, destroyed")
            .unitless().min(0).max(10'000);

    r.type<WorldSnapshotParams>("WorldSnapshotParams").doc("world.snapshot params")
        .field("name", &WorldSnapshotParams::name).doc("Snapshot name (an existing one is replaced)").required();
    r.type<WorldRestoreParams>("WorldRestoreParams").doc("world.restore params")
        .field("name", &WorldRestoreParams::name).doc("Name given to world.snapshot").required();

    r.enumeration<ImageFormat>("ImageFormat").doc("Inline image format")
        .value("jpeg", ImageFormat::jpeg, "JPEG, quality 85: the cheapest preview")
        .value("png", ImageFormat::png, "PNG: exact pixels");
    r.type<RenderCaptureParams>("RenderCaptureParams").doc("render.capture params")
        .field("path", &RenderCaptureParams::path).doc("PNG to write (full resolution), under the output directory")
        .field("hdr", &RenderCaptureParams::hdr)
            .doc("PFM of the frame's linear radiance, before exposure, post and tonemapping (deferred: the lit image; "
                 "path tracer: its accumulation, before any denoiser), under the output directory")
        .field("inline", &RenderCaptureParams::inlineImage).doc("Return a preview image (an MCP image block)")
        .field("format", &RenderCaptureParams::format).doc("The preview's format")
        .field("maxSize", &RenderCaptureParams::maxSize).doc("The preview's long side, at most").unit("px").min(16)
            .max(8192)
        .field("region", &RenderCaptureParams::region).doc("[x, y, w, h] of the frame kept, before scaling (zoom)")
            .unit("px").min(0).max(8192)
        .field("mode", &RenderCaptureParams::mode).doc("Render mode (a window host: deferred)")
        .field("spp", &RenderCaptureParams::spp).doc("Path tracer samples per pixel").unitless().min(1).max(65536)
        .field("bounces", &RenderCaptureParams::bounces)
            .doc("Path tracer bounces for this capture: 0 direct light only, 1 one bounce, ... (absent: the profile's)")
            .unitless().min(0).max(256)
        .field("frames", &RenderCaptureParams::frames)
            .doc("Frames rendered from a reset, the last one kept; 0: deferred 10, path tracer spp")
            .unitless().min(0).max(100'000)
        .field("width", &RenderCaptureParams::width).doc("Frame width; default 1920 (a window: its own)").unit("px")
            .min(16).max(8192)
        .field("height", &RenderCaptureParams::height).doc("Frame height; default 1080 (a window: its own)").unit("px")
            .min(16).max(8192)
        .field("denoise", &RenderCaptureParams::denoise).doc("Path tracer denoiser")
        .field("post", &RenderCaptureParams::post)
            .doc("Deferred post effects to turn on; absent: none")
        .field("ev100", &RenderCaptureParams::ev100)
            .doc("The camera's exposure, EV at ISO 100 (radiance L displays as L / (1.2 x 2^EV100)); absent: "
                 "render.settings'").unit("EV").min(-20.0).max(30.0)
        .field("tonemap", &RenderCaptureParams::tonemap).doc("The display curve; absent: render.settings'")
        .field("quality", &RenderCaptureParams::quality).doc("Deferred quality for this capture; absent: render.settings'")
        .field("timings", &RenderCaptureParams::timings).doc("Add the capture frame's per-pass GPU times")
        .field("hud", &RenderCaptureParams::hud).doc("Draw the combat HUD over the frame, as a window shows it (deferred)")
        .field("guiding", &RenderCaptureParams::guiding)
            .doc("Path guiding (path-traced modes): bounces also drawn toward where the grid learned light arrives "
                 "from; the same estimate, less noise from light reflected off surfaces")
        .field("caustics", &RenderCaptureParams::caustics)
            .doc("Caustics (rt_offline): light traced from each directional light through smooth surfaces onto "
                 "matte ones the camera sees; the same estimate, without the bright dots of sun glints");
    r.enumeration<TraceResolution>("TraceResolution").doc("A realtime ray-traced technique's resolution")
        .value("half", TraceResolution::half, "Half the frame's size, upsampled (the budgeted frame)")
        .value("full", TraceResolution::full, "The frame's size");
    r.enumeration<GeometryPath>("GeometryPath").doc("How the G-buffer draws")
        .value("auto", GeometryPath::automatic, "Meshlets for draws of 4,096 triangles or more, instances below")
        .value("meshlets", GeometryPath::meshlets, "Every draw with meshlets")
        .value("instances", GeometryPath::instances, "Every draw instanced");
    r.type<RtDirectSettings>("RtDirectSettings")
        .doc("Ray-traced direct light: ReSTIR DI over every light and emissive triangle (without it: a loop over the "
             "lights, cascaded shadow maps for the sun)")
        .field("enabled", &RtDirectSettings::enabled).doc("Resample and trace direct light (needs ray tracing)")
        .field("candidates", &RtDirectSettings::candidates).doc("Light samples a pixel draws a frame")
            .unitless().min(1).max(32)
        .field("spatial", &RtDirectSettings::spatial).doc("Realtime: neighbours whose samples a pixel reuses")
            .unitless().min(0).max(8)
        .field("resolution", &RtDirectSettings::resolution)
            .doc("Realtime: resampling and tracing resolution (half: a pixel of each 2x2 block; every pixel shades "
                 "two of the 4 nearest)");
    r.type<RtGISettings>("RtGISettings").doc("Ray-traced one-bounce indirect light")
        .field("enabled", &RtGISettings::enabled).doc("Trace RT GI (needs ray tracing); off: no indirect light")
        .field("resolution", &RtGISettings::resolution).doc("Realtime trace resolution")
        .field("rays", &RtGISettings::rays).doc("Realtime: rays a pixel").unitless().min(1).max(32);
    r.type<CsmSettings>("CsmSettings").doc("Cascaded shadow maps: the fallback without RT direct light")
        .field("cascades", &CsmSettings::cascades).doc("Cascades drawn").unitless().min(1).max(4)
        .field("size", &CsmSettings::size).doc("Each cascade's size").unit("px").min(512).max(4096);
    r.type<VfxSettings>("VfxSettings").doc("Effects' presentation switches (A/B captures)")
        .field("particles", &VfxSettings::particles).doc("Lit particles")
        .field("soft", &VfxSettings::soft).doc("Soft particle fade (off: hard)")
        .field("tracers", &VfxSettings::tracers).doc("Tracers")
        .field("decals", &VfxSettings::decals).doc("Decals")
        .field("smokeInGi", &VfxSettings::smokeInGi).doc("Smoke shades RT direct light and GI");
    r.type<RenderSettings>("RenderSettings").doc("The host's renderer settings; the defaults are the budgeted frame")
        .field("quality", &RenderSettings::quality).doc("Deferred quality")
        .field("rtDirect", &RenderSettings::rtDirect).doc("RT direct light")
        .field("rtGI", &RenderSettings::rtGI).doc("RT GI")
        .field("sss", &RenderSettings::sss).doc("Subsurface scattering")
        .field("geometry", &RenderSettings::geometry).doc("G-buffer geometry path")
        .field("occlusionCulling", &RenderSettings::occlusionCulling).doc("The late pass's Hi-Z occlusion test")
        .field("csm", &RenderSettings::csm).doc("Cascaded shadow maps")
        .field("ev100", &RenderSettings::ev100)
            .doc("The camera's exposure, EV at ISO 100 (radiance L displays as L / (1.2 x 2^EV100))").unit("EV")
            .min(-20.0).max(30.0)
        .field("tonemap", &RenderSettings::tonemap).doc("The display curve (every renderer and denoiser)")
        .field("autoExposure", &RenderSettings::autoExposure)
            .doc("A window's camera meters its frames (the log-average luminance to middle grey); captures use ev100")
        .field("exposureCompensation", &RenderSettings::exposureCompensation)
            .doc("EV added to what auto exposure meters (positive brightens)").unit("EV").min(-10.0).max(10.0)
        .field("vfx", &RenderSettings::vfx).doc("Effects' switches");
    r.type<RenderSettingsParams>("RenderSettingsParams").doc("render.settings params")
        .field("set", &RenderSettingsParams::set)
            .doc("Members to change (the rest keep theirs); absent: read only");
    r.type<RenderStatsParams>("RenderStatsParams").doc("render.stats params")
        .field("frames", &RenderStatsParams::frames)
            .doc("Medians over the newest this many finished frames (the renderer keeps 120)")
            .unitless().min(1).max(120)
        .field("passes", &RenderStatsParams::passes).doc("Include the per-pass GPU times");

    r.type<LogReadParams>("LogReadParams").doc("log.read params")
        .field("since", &LogReadParams::since).doc("Return records after this seq (the last read's next)").unitless()
            .min(0)
        .field("level", &LogReadParams::level).doc("Lowest level returned")
        .field("channel", &LogReadParams::channel).doc("Only this channel")
        .field("limit", &LogReadParams::limit).doc("Lines returned").unitless().min(1).max(1000);

    r.type<LevelInfoParams>("LevelInfoParams").doc("level.info params")
        .field("entity", &LevelInfoParams::entity).doc("The Level entity's id, 16 hex digits; absent: the only level");
    r.type<LevelReloadParams>("LevelReloadParams").doc("level.reload params")
        .field("entity", &LevelReloadParams::entity).doc("The Level entity's id, 16 hex digits; absent: the only level");
    // max has no field limits: outside (0, 1e5] is E_PARAMS, as a zero dir is.
    r.type<PhysicsRaycastParams>("PhysicsRaycastParams").doc("physics.raycast params")
        .field("from", &PhysicsRaycastParams::from).doc("Ray origin [x, y, z]").unit("m").required()
        .field("dir", &PhysicsRaycastParams::dir).doc("Direction [x, y, z], any nonzero length").unitless().required()
        .field("max", &PhysicsRaycastParams::max).doc("Farthest hit, in (0, 100000]").unit("m");
    r.type<PhysicsInfoParams>("PhysicsInfoParams").doc("physics.info params");
    r.type<EventsReadParams>("EventsReadParams").doc("events.read params")
        .field("since", &EventsReadParams::since)
            .doc("Events after this seq: 0 for all the history holds, else the previous reply's next").unitless().min(0)
        .field("max", &EventsReadParams::max).doc("Events in one reply (0: none, and next is the present)").unitless()
            .min(0).max(64)
        .field("names", &EventsReadParams::names)
            .doc("Channels to read, exact (physics.footstep) or a prefix (physics.*); absent: every channel").maxItems(32);
    r.type<PhysicsContactsParams>("PhysicsContactsParams").doc("physics.contacts params")
        .field("entity", &PhysicsContactsParams::entity).doc("Only the pairs this entity (16 hex digits) is in")
        .field("max", &PhysicsContactsParams::max).doc("Pairs in one reply").unitless().min(1).max(64);
    r.type<PlayerInfoParams>("PlayerInfoParams").doc("player.info params")
        .field("entity", &PlayerInfoParams::entity).doc("The player's id, 16 hex digits; absent: the only player");
    r.type<PlayerTeleportParams>("PlayerTeleportParams").doc("player.teleport params")
        .field("entity", &PlayerTeleportParams::entity).doc("The player's id, 16 hex digits; absent: the only player")
        .field("pos", &PlayerTeleportParams::pos).doc("Where its feet go, world [x, y, z]").unit("m")
            .min(-kMaxTeleport).max(kMaxTeleport).required()
        .field("yawDeg", &PlayerTeleportParams::yawDeg)
            .doc("Facing about +Y from -Z, counter-clockwise seen from above; absent: as it faced").unit("deg")
            .min(-360.0).max(360.0);

    r.type<CombatInfoParams>("CombatInfoParams").doc("combat.info params");
    r.type<AnimationInfoParams>("AnimationInfoParams").doc("animation.info params")
        .field("entity", &AnimationInfoParams::entity).doc("One animator, 16 hex digits; absent: every animator");
    r.type<AudioInfoParams>("AudioInfoParams").doc("audio.info params");
    r.type<VfxInfoParams>("VfxInfoParams").doc("vfx.info params")
        .field("since", &VfxInfoParams::since).doc("Records after this seq (the previous next)").unitless().min(0)
        .field("max", &VfxInfoParams::max).doc("Records in one reply").unitless().min(0).max(64);
    r.type<AudioMixParams>("AudioMixParams").doc("audio.mix params")
        .field("bus", &AudioMixParams::bus).doc("The bus that db, mute and solo set")
        .field("db", &AudioMixParams::db).doc("The bus's level in dB, replacing the game's").unitless().min(kMinMixDb)
            .max(kMaxMixDb)
        .field("mute", &AudioMixParams::mute).doc("Silence the bus's subtree")
        .field("solo", &AudioMixParams::solo).doc("Hear only soloed buses (with their parents and children)")
        .field("snapshot", &AudioMixParams::snapshot).doc("Force this snapshot; \"\" gives the schedule's back")
        .field("clear", &AudioMixParams::clear).doc("Drop every override first");
    r.type<CombatDamageParams>("CombatDamageParams").doc("combat.damage params")
        .field("entity", &CombatDamageParams::entity).doc("Who takes it, 16 hex digits (its Health or an ancestor's)")
            .required()
        .field("amount", &CombatDamageParams::amount).doc("Hit points to take").unitless().min(0.0).max(kMaxDamage)
            .required()
        .field("source", &CombatDamageParams::source).doc("Who dealt it, 16 hex digits");
}

} // namespace

const reflect::TypeRegistry& paramsRegistry() {
    static reflect::TypeRegistry r;
    static const bool once = (registerParams(r), true);
    (void)once;
    return r;
}

} // namespace ohao::api
