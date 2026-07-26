---
module: systems
id: examples
title: Examples map
standard: v2
---

## A scene is a `main()`

OHAO has no scene file format. There is no loader that turns a `.scene` into
actors, and nothing under `ohao/scene/` deserialises anything but a mesh asset.
A scene exists only as the C++ that constructs it — `createActor`, then a
`MeshComponent`, a `MaterialComponent`, a `LightComponent`.

`examples/` builds five non-interactive binaries, plus the GLFW `interactive`
viewer when GLFW is found. Four of the five are half scene description, half
harness. `cornell_box` writes its walls as raw `Vertex` quads and stacks twelve
coloured sphere lights in the box as a many-light stress case. `model_viewer`
builds a bedroom — bed, headboard, nightstand — around whatever model you pass,
sized from the model's own bounds by `SceneFramer`. `turntable` picks its room
from a mode string and orbits. `env_demo` drops a 50 m procedural ground quad
under the model so it stops floating in void.

Everything below — the `+ 3` loop, the shared argv parser, the destruction
order — describes these four inline drivers.

## The `+ 3` in every render loop

Every inline driver's RT path renders `samples + 3` frames, never `samples`. It
looks like superstition. It is exact — in one denoise mode out of five.

{{cite examples/cornell_box.cpp "const int frames = cli.useDeferred ? 10 : (samples + 3);"}}

The reason is the frame ring. `renderRTPipeline` opens by waiting on the fence
of the slot it is about to reuse and `memcpy`-ing that slot's mapped staging
buffer into the host pixel buffer — so what lands on the CPU is the image
submitted three `render()` calls ago, not the one about to be recorded.

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// Read back previous frame's pixels"}}

{{cite ohao/render/frame/frame_resources.hpp "constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;"}}

What accumulates is not the push-constant sample index. `pc.params.z` carries
`m_sampleIndex`, which seeds the sampler and nothing else. The accumulation
weight is `pc.control.y`, holding `m_historyFrameCount`, and the running sample
count lives in the alpha channel of the accumulation image, incremented by the
raygen itself. The two counters advance together at the bottom of every
dispatch, which is why the distinction stays invisible until something resets
one of them.

{{cite shaders/rt/pt_raygen.rgen "imageStore(accumBuffer, pixel, vec4(acc, count));"}}

Take `DenoiseMode::None` for the moment. Call $k$ (zero-based) leaves an image
carrying $k+1$ accumulated samples; the copy performed at the top of call $k$
carries call $k-3$'s. Let $N$ be the number of `render()` calls, $F$ the ring
depth, and $s_{\text{host}}$ the sample count of the image sitting in the host
buffer when the loop exits:

$$s_{\text{host}}(N) \;=\; \big((N-1)-F\big) + 1 \;=\; N - F, \qquad F = 3$$

Set $N = \text{spp} + 3$ and $s_{\text{host}}$ is exactly the spp the user asked
for. The deferred branch takes the same readback path — it accumulates nothing,
but it lags identically — so `cornell_box`'s ten deferred frames save the
seventh and `model_viewer`'s thirty save the twenty-seventh. Both ignore the spp
argument entirely.

{{cite examples/model_viewer.cpp "const int frames = cli.useDeferred ? 30 : (samples + 3);"}}

## Where the formula actually holds

`DenoiseMode` has five values — `None`, `OIDN`, `NRD`, `Atrous`, `DLSSRR` — and
$s_{\text{host}} = N - 3$ needs two things at once: the host image must come
from the lagged staging ring, *and* the raygen must be accumulating. Only
`None` gives you both.

`getPixels()` returns the lagged buffer for `None`, `Atrous` and `DLSSRR`.
`OIDN` and `NRD` abandon it and issue their own readback — a command buffer
submitted and waited on inside the getter, which therefore observes every queued
frame. OIDN's readback takes the accumulation image itself, whose RGB is the
running mean of every sample so far.

{{cite ohao/gpu/vulkan/renderer.cpp "if (!self->readbackHDRBuffers(beautyRGBA, albedoRGBA, normalRGBA, rw, rh)) {"}}

Accumulation splits the set along a different seam. `Atrous` and `DLSSRR` are
flagged `wants_fresh_sample`: the host forces `pc.control.y = 0` every frame,
the raygen takes its no-history branch, and the accumulation image is stored
with `count = 1.0` forever. Those denoisers own temporal accumulation
themselves, so handing them a converged image would double-filter it.

{{cite ohao/render/rt/rt_meta.hpp "static constexpr bool wants_fresh_sample ="}}

{{cite ohao/render/rt/path_tracer_render.cpp "pc.control.y = freshSample ? 0u : m_historyFrameCount;"}}

{{cite shaders/rt/pt_raygen.rgen "if (historyFrameCount == 0u) {"}}

The four non-`None` modes therefore miss in two directions. Under `Atrous` or
`DLSSRR` the saved PNG is 1 spp no matter what number you typed. Under `OIDN`
the fresh readback sees all $N$ submissions, so a `samples + 3` loop hands OIDN
a `samples + 3`-sample beauty — three more than requested, not fewer. `NRD`
returns its own GPU-tonemapped composite, for which the ring lag is irrelevant
in either direction.

The overshoot is the shipped default, not a corner case: the offline RT profile
sets `DenoiseMode::OIDN`, and no driver overrides it without `--denoise=`.

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::OIDN,"}}

So the `+ 3` is load-bearing in exactly one configuration and merely wasteful in
the rest. A driver that looped `samples` times would still be correct-looking
under the default and three samples short under `--denoise=none` — a bug that
surfaces only when you switch the denoiser off to check something.

## What the shared flag parser will not tell you

`parseRenderFlags` takes the trailing argv slice — from index 3 in
`cornell_box`, 4 in `model_viewer`, 5 in `turntable` — and matches tokens
order-independently. Unrecognised tokens fall off the end of the
`if`/`else if` chain with no diagnostic, so a typo'd `--denosie=none` renders
happily with the default denoiser.

`deferred` and `rt_realtime` are stored in different fields, and `resolveMode`
resolves the conflict by precedence rather than by order: `deferred` wins no
matter where it appears.

{{cite examples/example_cli.hpp "if (opts.useDeferred) return RenderMode::Deferred;"}}

The sample-count parser swallows anything `atoi` cannot turn into a positive
integer and returns the default, so `cornell_box out.png 0` quietly renders 1024
spp; it is also reused as a general positive-integer parser — `turntable` parses
its orbit frame count with it.

{{cite examples/example_cli.hpp "return v > 0 ? v : fallback;"}}

Nothing in the shared parser reports an unrecognised token, so a typo degrades
silently to the default rather than failing the run.

## env_demo imports the parser, then ignores it

`env_demo` carries `using`-declarations for all five shared helpers and calls
two of them. From `argv[5]` onward it runs its own parser.

{{cite examples/env_demo.cpp "for (int i = 5; i < argc; i++) {"}}

The trade is deliberate: it drops `deferred`, `video` and `outdoor`, re-implements
`flip` under a different spelling, and buys twelve `--dump-*` AOV taps, `--seq=K`
consecutive-frame capture, `--lighting=`, `--ground=`, `--aniso=`, `--sss=` and
`--crop=face`. The re-implementation is not quite a copy: the shared `flip`
rotates the mesh actors 180° about Y after framing, whatever the up-axis;
`env_demo`'s `--flip` folds the same 180° into the initial rotation and only in
the Y-up branch.

{{cite examples/env_demo.cpp "flipModel ? 180.0f : 0.0f"}}

That makes it the NRD/guide-buffer debugging instrument rather than a demo. It
is the only place the ViewZ, roughness, diffuse/specular radiance, hit-distance
and NRD-composed images become PNGs. Motion vectors it does not own: the GLFW
`interactive` viewer dumps `renders/mv_interactive.png` on the `M` key, through
a second, independently written copy of the same decoder.

{{cite examples/interactive.cpp "MV dumped: "}}

The surprise is the default. Lighting mode defaults to `Studio`, and studio mode
skips loading the HDR that `argv[2]` mandates:

{{cite examples/env_demo.cpp "if (lightingMode != LightingMode::Studio && lightingMode != LightingMode::Cinema) {"}}

So `env_demo model.glb env.hdr out.png 256` renders four neutral sphere lights
against a black background and never opens the `.hdr`. Only `--lighting=hdr-only`
and `--lighting=none` load it.

:::why
The note attached to the skip says the environment at full strength overwhelmed
the three-point rig and tinted surfaces with the env's own colour — "the
pink-ground problem in 4.H v1-v6". The choice made instead was to model a closed
studio: explicit lights, black background, which is what product photography
actually is. The cost is a mandatory argument that the default mode ignores.

The tree records a second, incompatible fix for the same symptom 130 lines
further down. The v8 lighting note blames the *warm key*, not the environment,
and fixes the pink ground by moving all four studio lights to D65 neutral white.
Both fixes shipped; nothing reconciles them. The env explanation is the rationale
attached to the skip, not a settled root cause.

{{cite examples/env_demo.cpp "// v8: D65 neutral white lights"}}
:::

One more artefact of that instrument role: `readbackMotionVector` hands back raw
`RG16F` halves while `readbackRoughnessAOV` decodes in the helper, so `env_demo`
carries a hand-written IEEE-754 half-to-float — denormal renormalisation loop
included — used by nothing but the motion-vector PNG.

{{cite examples/env_demo.cpp "while ((mant & 0x400) == 0) { mant <<= 1; exp--; }"}}

## The orbit reset, and what breaks if you tighten the loop

`turntable` moves the camera to the next orbit position, resets accumulation,
renders `spp + 3` frames, writes the PNG, repeats.

{{cite examples/turntable.cpp "renderer.resetAccumulation();"}}

The reset must sit after the camera move and before the inner loop, or the
accumulation buffer keeps radiance from the previous viewpoint. But the reset
clears the path tracer's sample index and history — it does not touch the
staging ring. The first three copies of orbit position $f$ still deliver the
tail of position $f-1$. That is why the `+ 3` cannot be trimmed here either: at
`spp` iterations the written PNG would be three samples short under
`--denoise=none`, and for `spp` of 3 or less it would be the *previous* camera
position's image — a turntable visibly lagging its own orbit, produced entirely
by the driver.

Of the three drivers that can resolve to `Deferred`, `turntable` is the only one
whose loop does not special-case it. It renders `spp + 3` frames per position
whatever `resolveMode` returned, and its banner prints `RTOffline` for any mode
that is not `RTRealtime` — including `Deferred`.

{{cite examples/turntable.cpp "(rtMode == RenderMode::RTRealtime ? "}}

## One model, N actors

`model_viewer` splits a multi-material model into one actor per material. The
regroup rebuilds geometry per triangle: three fresh vertices pushed for every
triangle, indices numbered consecutively. The sub-model comes out fully
unwelded — `vertexCount = 3 × triangleCount` — so a shared vertex is re-emitted
once per incident triangle, which on a closed manifold is its valence, about
six.

{{cite examples/model_viewer.cpp "subModel->vertices.push_back(model->vertices[oldIdx]);"}}

Worth knowing before copying that pattern: neither of the two render paths
`model_viewer` can actually select still requires the split. The RT material
table already carries per-triangle material IDs, offset per model, and the
GBuffer pass already subdivides each actor's index range into per-material draw
ranges.

{{cite ohao/render/deferred/gbuffer_pass.cpp "drawRanges = buildMaterialDrawRanges(*model, bufferInfo);"}}

The path that genuinely takes one material per actor is the forward/legacy
rasteriser, which reads a single `MaterialComponent` and pushes it as one push
constant per draw.

{{cite ohao/gpu/vulkan/scene_upload.cpp "auto materialComp = actor->getComponent<MaterialComponent>();"}}

`render()` reaches that path when the RT guard fails — no RT pipeline for the
mode, no RT renderer, or no acceleration structure — and the deferred guard
fails as well. That second guard tests the renderer's existence, not just the
mode, so it is `m_deferredRenderer` that decides, not `RenderMode::Deferred`
alone.

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_renderMode == RenderMode::Deferred && m_deferredRenderer) {"}}

The reachable case is a device without ray-tracing support: `setRenderMode`
refuses the switch, the mode stays `Forward`, and an `rt_offline` request
silently renders through the legacy rasteriser. So the split is insurance for
the no-RT fallback, paid for in vertex count and BLAS entries on every run.

:::key
The frame you last rendered is not the frame you saved. Both the RT and the
deferred readback copy the staging buffer of the slot they are about to reuse,
so the host image trails by `MAX_FRAMES_IN_FLIGHT` calls. Every `+ 3` in the
inline drivers is that constant — and it converts spp into an exact sample count
only under `--denoise=none`, because every other mode either bypasses the lagged
buffer or refuses to accumulate into it.
:::

## Contracts

- Under `--denoise=none`, `render()` must be called `spp + MAX_FRAMES_IN_FLIGHT` times to land `spp` accumulated samples in the host buffer. The same loop hands OIDN `spp + 3` samples and `Atrous`/`DLSSRR` exactly 1. If the ring depth ever changes, every driver's `+ 3` is wrong and only `--denoise=none` output will show it.
- Camera move → `resetAccumulation()` → render. Reversing the last two mixes viewpoints in the accumulation buffer; skipping the reset entirely does the same.
- The `Scene` must be destroyed before the `VulkanRenderer`. All four inline drivers declare the renderer first and the scene second, so reverse-order destruction of locals gets it right; `cornell_box` and `model_viewer` also call `scene.reset()` explicitly. Swapping those two declarations tears down Vulkan resources still referenced by actors.
- `deferred` overrides `rt_realtime`/`rt_offline` regardless of argument order in the three non-interactive drivers that use the shared parser. What follows from that is per-driver: `cornell_box` and `model_viewer` then ignore the spp argument for a fixed 10 and 30 frames, `turntable` keeps rendering `spp + 3` per orbit position, and `env_demo` has no `deferred` token at all.
- An unknown trailing flag is silently ignored by `parseRenderFlags` and by `env_demo`'s own parser — typos degrade to defaults rather than failing the run.
