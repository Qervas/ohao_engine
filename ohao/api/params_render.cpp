// The session's params registry (api/params.hpp), a part of params.cpp's: render.capture.
#include "api/params.hpp"
#include "ecs/scene_types.hpp"   // registerKeyEnum
#include <limits>

namespace ohao::api {

void registerParamsRender(reflect::TypeRegistry& r) {
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
}

} // namespace ohao::api
