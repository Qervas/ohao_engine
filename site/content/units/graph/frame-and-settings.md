---
module: graph
id: frame-and-settings
title: The frame, measured and set
standard: v2
---

## One list of passes

Every GPU pass a frame can run is one entry of `kPasses` in
`render/frame/pass_registry.hpp`, in frame order: a name, a kind (transfer, raster,
compute, ray tracing) and a line saying what it does. The pass timer records only passes
named there, so nothing runs unmeasured and nothing is measured under two names; the
same names are what `render.stats` and a capture's `timings` report and what
`tests/bench/render_budgets.json` budgets.

{{cite ohao/render/frame/pass_registry.hpp "inline constexpr PassInfo kPasses[] = {"}}

A deferred frame runs `scene.upload`, then the culling and G-buffer pairs
(`cull.instances`, `gbuffer.early`, `hiz.build`, `cull.late`, `gbuffer.late`),
`lights.cluster`, the screen-space and ray-traced inputs to lighting (`ssao`,
`rt.shadows` and `rt.shadows.filter` or else `csm`, `rt.gi` and `rt.gi.denoise`),
`lighting`, `sky`, `particles`, `sss`, `ssr`, the post stack (`post.bloom`, `post.taa`,
`post.tonemap`), `gizmo`, and the way out: `capture.preview` during an inline capture,
then `present` in a window or `readback` headless. A path-traced frame is
`scene.upload`, `pt.render` and `pt.present` or `pt.readback`.

{{cite ohao/render/frame/pass_registry.hpp "{"present", PassKind::Transfer, "the final image blitted to the swapchain, and a requested readback copy"},"}}

:::why
The registry is the order as well as the names because the timer's query pairs are
written in the order passes record, and a pass recorded out of registry order is a
bug that `engine_tests` catches. A budget file keyed by the same strings means a
renamed pass fails its budget instead of quietly escaping it.
:::

## What a capture says it cost

A capture reads its last frame back after that frame's fence has signalled, so the
frame's timestamps are already resolved when the pixels are. The capture reports the
frame's GPU span as `gpuMs`, and with `timings` each pass in frame order: the same
numbers `render.stats {frames: 1}` would give for that frame, without a second round
trip.

{{cite ohao/runtime/frame_capture.cpp "out.timings.emplace_back(std::string(render::kPasses[f.pass[i]].name), f.ms[i]);"}}

The record runs from a destructor at the end of `readCaptureOutputs`, so every return
path out of the readback, an early error included, still fills in what was measured.

## Settings: the frame's knobs as host state

`VulkanRenderer::RenderSettings` gathers the knobs that change what a frame runs: the
quality (realtime or reference), RT shadows (on, at most 0–4 lights a pixel), RT GI (on,
half or full resolution, 1–32 rays), SSAO, SSR, SSS, the geometry path (auto, meshlets,
instances), occlusion culling, the CSM fallback's cascades (1–4) and map size
(512–4096), and exposure. Its defaults are the frame the budgets were set for. Setting
them hands each value to the subsystem that owns it; a pass turned off is simply not
registered in the frame's graph, so it costs nothing and disappears from the timings.

{{cite ohao/gpu/vulkan/renderer.cpp "if (s.csmCascades != d.csmCascades() || s.csmSize != d.csmSize()) d.configureCSM(s.csmCascades, s.csmSize);"}}

Reading them back asks the same subsystems, never a cached copy, so what
`render.settings` answers is what the next frame will do.

{{cite ohao/gpu/vulkan/renderer.cpp "s.csmCascades = d.csmCascades();"}}

Two changes rebuild images: the GI resolution (its trace and history images change size)
and the CSM map size. Both idle the device once, because the images being replaced may
still be read by frames in flight; every other change applies on the next frame.

:::why
Settings are host state rather than per-capture parameters so a window shows what a
capture will render, `render.stats` measures the configuration an agent is about to
judge, and a session's captures stay comparable. A capture still overrides the three
that matter for a single image (`post`, `exposure`, `quality`) and otherwise takes them
from the settings.
:::

## Over the API

`render.settings {set?}` reads the settings, or merges a partial `set` into them: the
reflected params struct is decoded onto the current values, so members left out keep
theirs, and the limits are checked before anything is applied (`E_RANGE`, nothing
changed). A set counts as a change of the session. The CLI host renders nothing between
captures and has no settings (`E_STATE`); a headless `ohao serve` keeps settings set
before its first capture and applies them when the renderer starts.

{{cite ohao/api/session.cpp "{"render.settings", renderSettings}"}}
