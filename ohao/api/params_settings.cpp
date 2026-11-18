// The session's params registry (api/params.hpp), a part of params.cpp's: render.settings to audio.mix.
#include "api/params.hpp"
#include "ecs/scene_types.hpp"   // registerKeyEnum
#include <limits>

namespace ohao::api {

void registerParamsSettings(reflect::TypeRegistry& r) {
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
        .field("vfx", &RenderSettings::vfx).doc("Effects' switches")
        .field("renderScale", &RenderSettings::renderScale)
            .doc("The fraction of the output's width and height the deferred frame is rendered at; TAA upscales it "
                 "(1: none). The frame's GPU time follows the pixels rendered")
            .unitless().min(0.5).max(1.0);
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

    r.type<AssetInfoParams>("AssetInfoParams").doc("asset.info params");
    r.type<AssetReloadParams>("AssetReloadParams").doc("asset.reload params")
        .field("path", &AssetReloadParams::path).doc("A model file's project-relative path; absent: every file");
    r.type<EventsReadParams>("EventsReadParams").doc("events.read params")
        .field("since", &EventsReadParams::since)
            .doc("Events after this seq: 0 for all the history holds, else the previous reply's next").unitless().min(0)
        .field("max", &EventsReadParams::max).doc("Events in one reply (0: none, and next is the present)").unitless()
            .min(0).max(64)
        .field("names", &EventsReadParams::names)
            .doc("Channels to read, exact (physics.footstep) or a prefix (physics.*); absent: every channel").maxItems(32);

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
}

} // namespace ohao::api
