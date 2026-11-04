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
    {"skin", PassKind::Compute, "skinned instances whose pose changed: each vertex through its palette into the instance's own mesh, last frame's position beside it (skin/skin.slang)"},
    {"skin.bounds", PassKind::Compute, "the skinned instances' meshlet spheres around their deformed vertices"},
    {"rt.blas.refit", PassKind::RayTracing, "the skinned instances' BLASes refit to their pose (rebuilt every kBlasRefitsPerRebuild refits)"},
    {"scene.upload", PassKind::Transfer, "RenderScene table uploads, BLAS builds, the TLAS build or refit"},
    {"csm", PassKind::Raster, "cascaded shadow maps (render.settings csm, default 4 of 2048x2048): only when RT direct light does not run"},
    {"cull.instances", PassKind::Compute, "GPU culling, early phase: the draws visible last frame and in the frustum, as indirect lists (prefix-sum compaction)"},
    {"gbuffer.early", PassKind::Raster, "the G-buffer (position, normal, albedo, velocity, depth) cleared, and the early lists drawn"},
    {"hiz.build", PassKind::Compute, "the max-depth pyramid of the early pass's depth"},
    {"cull.late", PassKind::Compute, "GPU culling, late phase: every draw against the frustum and the pyramid; the newly visible listed, visibility recorded"},
    {"gbuffer.late", PassKind::Raster, "the late lists drawn into the G-buffer"},
    {"decals", PassKind::Raster, "projected decals into the G-buffer's albedo and normal, over the surfaces inside their boxes (vfx/decal.slang)"},
    {"lights.cluster", PassKind::Compute, "clustered lighting: every froxel's (16x9 tiles x 24 slices) list of the lights that reach it"},
    {"emissive.lights", PassKind::Compute, "the emissive triangle lights (path tracer and RT direct light): each triangle and its power, the power image's levels"},
    {"di.sample", PassKind::RayTracing, "ReSTIR DI at the trace resolution (render.settings rtDirect.resolution, default half): rtDirect.candidates light points a pixel by RIS, last frame's reservoir, a visibility ray"},
    {"di.trace", PassKind::RayTracing, "ReSTIR DI at the trace resolution: rtDirect.spatial neighbours' reservoirs, one shadow ray to the final point"},
    {"di.shade", PassKind::Compute, "ReSTIR DI: every pixel's diffuse and specular light from the trace pixels around it, through its own material"},
    {"di.denoise", PassKind::Compute, "ReSTIR DI's reprojected temporal accumulation and 3 a-trous iterations (realtime)"},
    {"rt.gi", PassKind::RayTracing, "ray-traced indirect light, every bounce through its radiance cache (and the cache's resolve), and a reflection ray a pixel: render.settings rtGI, default half resolution, 2 diffuse rays a pixel (reference: full, 32)"},
    {"rt.gi.denoise", PassKind::Compute, "RT GI's reprojected temporal accumulation, 2 a-trous iterations and joint-bilateral upsample, for the diffuse and the reflection (its spread by roughness; realtime)"},
    {"lighting", PassKind::Raster, "deferred lighting into the HDR image"},
    {"vfx.expand", PassKind::Compute, "every live effect particle in closed form from its record and age, lit at its centre (vfx/expand.slang)"},
    {"vfx.sort", PassKind::Compute, "the alpha-blended effect batches' particles back to front, by integer keys (vfx/sort.slang)"},
    {"sss", PassKind::Compute, "subsurface scattering blur (over the lit image with the sky)"},
    {"transparent", PassKind::Raster, "blended surfaces and effects back to front over the lit image, soft against depth, coverage into TAA's mask"},
    {"post.bloom", PassKind::Raster, "bloom (the bloom post effect)"},
    {"post.taa", PassKind::Raster, "temporal anti-aliasing (the taa post effect)"},
    {"exposure", PassKind::Compute, "a window's auto exposure: the frame's luminance histogram and the camera's adaptation"},
    {"post.tonemap", PassKind::Raster, "HDR to the RGBA8 final image"},
    {"gizmo", PassKind::Raster, "transform handles"},
    {"hud", PassKind::Raster, "the HUD over the final image (a window always; a capture with hud: true)"},
    {"capture.preview", PassKind::Compute, "only during an inline capture: the final image box-filtered to the preview's size, into the frame's staging buffer"},
    {"present", PassKind::Transfer, "the final image blitted to the swapchain, and a requested readback copy"},
    {"readback", PassKind::Transfer, "headless: the final image copied to the frame's staging buffer"},
    {"pt.cache.update", PassKind::RayTracing, "the realtime path tracer: the radiance cache's sparse update trace (and the frame's setup)"},
    {"pt.cache.resolve", PassKind::Compute, "the realtime path tracer: the radiance cache's blend and eviction"},
    {"pt.manifold.gather", PassKind::Compute, "caustics step 2: the owned casters' triangles into world space, in clusters and groups with boxes and normal cones"},
    {"pt.caustics", PassKind::RayTracing, "caustics: rays from each directional light through smooth surfaces, splatted where they land on a matte surface the camera sees"},
    {"pt.trace", PassKind::RayTracing, "the path tracer's trace: a sample a pixel, accumulated or handed to the denoiser"},
    {"pt.manifold", PassKind::RayTracing, "caustics step 2: each request's sun reflected by a smooth surface onto its rough point, solved on the casters' triangles, added to the running mean"},
    {"pt.guide.update", PassKind::Compute, "path guiding: the frame's proposals learned into the grid the next frame reads (update, owner, write, the copy)"},
    {"pt.denoise", PassKind::Compute, "the path tracer's denoiser (NRD, DLSS RR) and display transform"},
    {"pt.present", PassKind::Transfer, "a window: the path tracer's output blitted to the swapchain, and a requested readback copy"},
    {"pt.readback", PassKind::Transfer, "headless: the path tracer's output copied to the frame's staging buffer"},
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
