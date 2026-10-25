#pragma once
// Every GPU pass a frame can run, in frame order (sub-project 2b spec, "The frame after
// 2b"). The pass timer records only passes named here, render.stats and render_bench report
// them by these names, and tests/bench/render_budgets.json budgets them by these names, so a
// pass cannot run unmeasured or be measured under two names. A new pass gets an entry here
// (and a budget) before it gets a timer.
#include <cstdint>
#include <string_view>

namespace ohao::render {

enum class PassKind : uint8_t { Transfer, Raster, Compute, RayTracing };

struct PassInfo {
    std::string_view name;
    PassKind kind;
    std::string_view what;
};

inline constexpr PassInfo kPasses[] = {
    {"scene.upload", PassKind::Transfer, "RenderScene table uploads, BLAS builds, the TLAS build or refit"},
    {"csm", PassKind::Raster, "cascaded shadow maps (4 of 2048x2048): only when RT shadows do not run"},
    {"gbuffer", PassKind::Raster, "the G-buffer: position, normal, albedo, velocity, depth"},
    {"ssao", PassKind::Compute, "screen-space ambient occlusion (the ssao post effect)"},
    {"rt.shadows", PassKind::RayTracing, "ray-traced shadows: one ray to each of a pixel's most important lights (reference: 8 to every light)"},
    {"rt.shadows.filter", PassKind::Compute, "RT shadows' 5x5 depth- and normal-aware filter and temporal accumulation (realtime)"},
    {"rt.gi", PassKind::RayTracing, "ray-traced one-bounce indirect light: half resolution, 2 rays a pixel (reference: full, 32)"},
    {"rt.gi.denoise", PassKind::Compute, "RT GI's reprojected temporal accumulation, 2 a-trous iterations and joint-bilateral upsample (realtime)"},
    {"lighting", PassKind::Raster, "deferred lighting into the HDR image"},
    {"sky", PassKind::Raster, "the procedural sky into the HDR image"},
    {"particles", PassKind::Raster, "particle update (compute) and draw"},
    {"sss", PassKind::Compute, "subsurface scattering blur (over the lit image with sky and particles)"},
    {"ssr", PassKind::Compute, "screen-space reflections"},
    {"post.bloom", PassKind::Raster, "bloom (the bloom post effect)"},
    {"post.taa", PassKind::Raster, "temporal anti-aliasing (the taa post effect)"},
    {"post.tonemap", PassKind::Raster, "HDR to the RGBA8 final image"},
    {"gizmo", PassKind::Raster, "transform handles"},
    {"capture.preview", PassKind::Compute, "only during an inline capture: the final image box-filtered to the preview's size, into the frame's staging buffer"},
    {"present", PassKind::Transfer, "the final image blitted to the swapchain, and a requested readback copy"},
    {"readback", PassKind::Transfer, "headless: the final image copied to the frame's staging buffer"},
    {"pt.render", PassKind::RayTracing, "the path tracer: trace, accumulate, denoise, tonemap"},
    {"pt.readback", PassKind::Transfer, "the path tracer's output copied to the frame's staging buffer"},
};

inline constexpr int kPassCount = static_cast<int>(sizeof(kPasses) / sizeof(kPasses[0]));

// The registry index of a pass name, or -1.
[[nodiscard]] constexpr int passIndex(std::string_view name) noexcept {
    for (int i = 0; i < kPassCount; ++i)
        if (kPasses[i].name == name) return i;
    return -1;
}

[[nodiscard]] constexpr std::string_view passKindName(PassKind k) noexcept {
    switch (k) {
    case PassKind::Transfer: return "transfer";
    case PassKind::Raster: return "raster";
    case PassKind::Compute: return "compute";
    case PassKind::RayTracing: return "rt";
    }
    return "?";
}

} // namespace ohao::render
