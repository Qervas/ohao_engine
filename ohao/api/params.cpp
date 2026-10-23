#include "api/params.hpp"

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
        .value("atrous", Denoiser::atrous, "A-trous wavelet filter")
        .value("dlss", Denoiser::dlss, "DLSS Ray Reconstruction");
    r.enumeration<PostEffect>("PostEffect").doc("Deferred post effect")
        .value("bloom", PostEffect::bloom, "Bloom")
        .value("taa", PostEffect::taa, "Temporal anti-aliasing")
        .value("ssao", PostEffect::ssao, "Screen-space ambient occlusion");
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
        .value("runtime", LogChannelName::runtime, "Window and main loop");

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
            .required();

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
    r.type<WorldDiffParams>("WorldDiffParams").doc("world.diff params")
        .field("snapshot", &WorldDiffParams::snapshot).doc("Name given to world.snapshot").required()
        .field("fields", &WorldDiffParams::fields).doc("Only these: \"/Type\" or \"/Type/field\"; absent: everything")
        .field("limit", &WorldDiffParams::limit).doc("Entries listed in each of changed, created, destroyed")
            .unitless().min(0).max(10'000);

    r.type<WorldSnapshotParams>("WorldSnapshotParams").doc("world.snapshot params")
        .field("name", &WorldSnapshotParams::name).doc("Snapshot name (an existing one is replaced)").required();
    r.type<WorldRestoreParams>("WorldRestoreParams").doc("world.restore params")
        .field("name", &WorldRestoreParams::name).doc("Name given to world.snapshot").required();

    r.type<RenderCaptureParams>("RenderCaptureParams").doc("render.capture params")
        .field("path", &RenderCaptureParams::path).doc("PNG to write, under the output directory").required()
        .field("mode", &RenderCaptureParams::mode).doc("Render mode")
        .field("spp", &RenderCaptureParams::spp).doc("Path tracer samples per pixel").unitless().min(1).max(65536)
        .field("frames", &RenderCaptureParams::frames).doc("Frames rendered; 0: deferred 10, path tracer spp + 3")
            .unitless().min(0).max(100'000)
        .field("width", &RenderCaptureParams::width).doc("Image width").unit("px").min(16).max(8192)
        .field("height", &RenderCaptureParams::height).doc("Image height").unit("px").min(16).max(8192)
        .field("denoise", &RenderCaptureParams::denoise).doc("Path tracer denoiser")
        .field("post", &RenderCaptureParams::post).doc("Deferred post effects to turn on")
        .field("exposure", &RenderCaptureParams::exposure).doc("Exposure in (0, 64]; default the renderer's (0.6)")
            .unitless().min(0.0).max(64.0);

    r.type<LogReadParams>("LogReadParams").doc("log.read params")
        .field("since", &LogReadParams::since).doc("Return records after this seq (the last read's next)").unitless()
            .min(0)
        .field("level", &LogReadParams::level).doc("Lowest level returned")
        .field("channel", &LogReadParams::channel).doc("Only this channel")
        .field("limit", &LogReadParams::limit).doc("Lines returned").unitless().min(1).max(1000);
}

} // namespace

const reflect::TypeRegistry& paramsRegistry() {
    static reflect::TypeRegistry r;
    static const bool once = (registerParams(r), true);
    (void)once;
    return r;
}

} // namespace ohao::api
