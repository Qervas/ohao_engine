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
    {"csm", PassKind::Raster, "cascaded shadow maps: 4 cascades of 2048x2048, every draw"},
    {"gbuffer", PassKind::Raster, "the G-buffer: position, normal, albedo, velocity, depth"},
    {"ssao", PassKind::Compute, "screen-space ambient occlusion (the ssao post effect)"},
    {"rt.shadows", PassKind::RayTracing, "ray-traced shadow mask"},
    {"rt.gi", PassKind::RayTracing, "ray-traced one-bounce indirect light and its history copy"},
    {"lighting", PassKind::Raster, "deferred lighting into the HDR image"},
    {"sss", PassKind::Compute, "subsurface scattering blur"},
    {"ssr", PassKind::Compute, "screen-space reflections"},
    {"sky", PassKind::Raster, "the procedural sky into the HDR image"},
    {"particles", PassKind::Raster, "particle update (compute) and draw"},
    {"post.bloom", PassKind::Raster, "bloom (the bloom post effect)"},
    {"post.taa", PassKind::Raster, "temporal anti-aliasing (the taa post effect)"},
    {"post.tonemap", PassKind::Raster, "HDR to the RGBA8 final image"},
    {"gizmo", PassKind::Raster, "transform handles"},
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
