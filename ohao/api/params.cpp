#include "api/params.hpp"

#include "ecs/scene_types.hpp"   // registerKeyEnum

#include <limits>

namespace ohao::api {

// The parts of these registrations, each its own unit (they compile in parallel), in this order.
void registerParamsWorld(reflect::TypeRegistry& r);      // world.apply to world.restore (params_world.cpp)
void registerParamsRender(reflect::TypeRegistry& r);     // render.capture (params_render.cpp)
void registerParamsSettings(reflect::TypeRegistry& r);   // render.settings to audio.mix (params_settings.cpp)

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
    registerParamsWorld(r);
    registerParamsRender(r);
    registerParamsSettings(r);
}

} // namespace

const reflect::TypeRegistry& paramsRegistry() {
    static reflect::TypeRegistry r;
    static const bool once = (registerParams(r), true);
    (void)once;
    return r;
}

} // namespace ohao::api
