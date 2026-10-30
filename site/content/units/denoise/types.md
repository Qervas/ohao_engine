---
module: denoise
id: types
title: Denoise modes and traits
standard: v2
figures: [denoise-types-modeflow]
---

## Five values, numbered with a gap

`DenoiseMode` is the single token every part of the RT stack uses to say which
denoiser is in play, and its enumerators are numbered explicitly and
non-contiguously. Value 2 is missing: the OptiX backend was deleted (it
segfaulted on shutdown), and the survivors kept their old numbers rather than
closing the gap, so NRD is still 3 and the two later additions took 4 and 5.

{{cite ohao/render/rt/denoise/denoise_types.hpp "NRD    = 3,   // NVIDIA RayTracingDenoiser (Sub-plan 4)"}}

The gap costs nothing, because the numeric value never crosses a boundary:
nothing packs a `DenoiseMode` into a push constant, writes it to a file, or casts
an integer back into one. `denoiseModeIndex` is the only function that reads the
underlying value at all, and it has no callers. The numbering is source hygiene,
not an ABI.

The parser degrades instead of failing. `optix` still parses, to OIDN, with a
warning on stderr, so old scripts keep running:

{{cite ohao/render/rt/denoise/denoise_types.cpp "optix is no longer supported"}}

More importantly, `nrd` and `dlssrr` are gated on the build defines that decide
whether those backends were compiled in at all; if they weren't, the string
resolves to `None` rather than to a mode whose implementation is absent.

{{cite ohao/render/rt/denoise/denoise_types.cpp "requested but OHAO_DLSS=OFF at build time"}}

:::why
What the fallback buys is mechanical, and only the mechanism is recorded — no
comment or commit message weighs an alternative. The frame-final `readback()`
dispatches on the mode, and the NRD arm reads back the binding-30 tonemapped
image:

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_denoiseMode == DenoiseMode::NRD) {"}}

That image's allocation carries no build guard, but everything that *writes* it
lives inside one:

{{cite ohao/render/rt/path_tracer_render.cpp "#ifdef OHAO_NRD_ENABLED"}}

So an `NRD` mode reaching a non-NRD build would not error: the readback finds a
live `VkImage`, returns success, and hands back contents nothing ever wrote.
Degrading at parse time keeps that mode out of the switch entirely.
:::

## What each denoiser asks of the trace

`rt_meta.hpp` states the per-mode requirements once, as plain `constexpr`
functions of a `DenoiseMode` (or of a whole `RTRenderSettings`), usable both in
constant expressions and on a mode that arrived from `argv`:

{{cite ohao/render/rt/rt_meta.hpp "[[nodiscard]] constexpr bool denoiseNeedsMotionVectors(DenoiseMode m) noexcept {"}}

Each is a direct predicate: motion vectors for NRD, à-trous and DLSS RR; the
diffuse/specular split for NRD; a CPU readback for OIDN; sub-pixel jitter for NRD
and DLSS RR. There is no `switch` with a `default:` arm and no trait class behind
them, so a sixth enumerator answers `false` to every requirement until a predicate
names it, and no diagnostic says so.

Two of the functions take the settings rather than the mode, because the answer
depends on the profile as well:

{{cite ohao/render/rt/rt_meta.hpp "[[nodiscard]] constexpr bool tracerAccumulates(const RTRenderSettings& s) noexcept {"}}

`tracerAccumulates` says whether the trace keeps its own running mean of the
beauty. À-trous and DLSS RR never let it: each is temporal and carries the
history itself, taking this frame's sample alone. NRD lets it only offline, where
it denoises the mean of a still view's samples; realtime NRD reprojects its own
history through the motion vectors. `None` and `OIDN` always accumulate.
`tracerAccumulatesAOVs` is the matching question for NRD's five guide images:
true only for offline NRD.

## What the policy gates

Three answers reach the raygen inside the tracer's per-frame record, all as
push-constant state rather than as pipeline selection. Sub-pixel jitter is the
first: for `NRD` and `DLSSRR`, the tracer offsets the sample grid by a
Halton(2,3) point each frame so the denoiser's temporal accumulation integrates a
wider footprint than centre-sampling would.

{{cite ohao/render/rt/path_tracer_render.cpp "if (denoiseNeedsPixelJitter(m_renderSettings.denoiseMode)) {"}}

The predicate is an enumerated pair, not a property. `Atrous` owns temporal
reprojection too — it is SVGF, with a reprojection pass and a persistent history
buffer — and is still excluded; the else arm groups it with `None` and `OIDN` on
pixel centres, the stated reason being parity:

{{cite ohao/render/rt/path_tracer_render.cpp "None/OIDN/Atrous keep pixel-center sampling"}}

The sequence index is bumped by one before use, because Halton's zeroth term is
exactly 0 — indistinguishable from "no jitter", and it would waste one of the 16
slots in the period:

{{cite ohao/render/rt/path_tracer_render.cpp "const uint32_t idx = (m_haltonIndex % 16u) + 1u;"}}

Second, `tracerAccumulates` sets how many frames the raygen's running mean
already holds. The tracer keeps two clocks: `m_historyFrameCount`, the denoisers'
history, which only `resetAccumulation` restarts, and `m_accumFrames`, the
trace's own mean, which a moved view restarts. The push constant carries the
second, or 0 when a temporal denoiser takes this frame's sample alone:

{{cite ohao/render/rt/path_tracer_render.cpp "pc.control.y = tracerAccumulates(m_renderSettings) ? m_accumFrames : 0u;"}}

The same answer gates the firefly clamp. The clamp is biased, and a running mean
converges without it, so it is applied only when the trace does not accumulate:

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_renderSettings.enableFireflyClamp && !tracerAccumulates(m_renderSettings))"}}

Third, `tracerAccumulatesAOVs` sets a raygen control bit that makes the five NRD
guide images hold the running mean of the accumulated frames instead of the last
one, so offline `--spp N` feeds NRD an N-sample mean:

{{cite ohao/render/rt/path_tracer_render.cpp "if (tracerAccumulatesAOVs(m_renderSettings)) pc.control.x |= kPTFlagAccumulateAOVs;"}}

The denoisers' own resets read the other clock: à-trous, DLSS RR and NRD restart
their history only when `m_historyFrameCount` is 0, so a moved view reprojects
through the motion vectors instead of starting over.

{{cite ohao/render/rt/path_tracer_render.cpp "camera.frameIndex = m_historyFrameCount;   // 0 only after resetAccumulation: a moved view reprojects"}}

## The copy the tracer reads, and the override

The mode is stored twice. `--denoise` writes `VulkanRenderer::m_denoiseMode` and
latches an override flag; the path tracer reads its own
`RTRenderSettings::denoiseMode`, which is reset to the active profile's defaults
at the top of every RT frame:

{{cite ohao/gpu/vulkan/renderer.cpp "m_rtSettings = defaults;"}}

`applyRTRenderSettings` then re-injects the user's choice, whatever it is —
`None` and `OIDN` included, because the tracer's running mean and firefly clamp
follow the mode:

{{cite ohao/gpu/vulkan/renderer.cpp "m_rtSettings.denoiseMode = m_denoiseMode;"}}

Without an override, the profile default stands: NRD for the realtime profile,
OIDN for the offline one. OIDN runs on the CPU inside `readback()` and dispatches
nothing in the tracer. NRD's realtime input is one frame's sample of paths that end in
the radiance cache past their first vertex (the profile's `radianceCache`), so the
denoiser sees the cache's light at two bounces' cost; an override of the denoiser does
not change that, since the cache is a profile setting, not a denoise one.

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::NRD,   // a moving view is one sample a frame: the denoiser keeps the history"}}
{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::OIDN,"}}

{{figure denoise-types-modeflow "Conceptual data flow, not measured: how the parsed mode reaches its two storage sites and which readers consult each. Box contents are read from renderer.cpp, rt_settings.hpp and path_tracer_render.cpp."}}

The NRD chain is gated on the mode, so it runs only when NRD was chosen:

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs && m_renderSettings.denoiseMode == DenoiseMode::NRD) {"}}

:::key
`DenoiseMode` decides three things: which GPU denoiser the tracer dispatches
(à-trous, DLSS RR, NRD — none of them run unasked), whether the trace keeps its
own running mean (and with it the firefly clamp), and which image `readback()`
hands back. The profile default fills it when the user names no mode.
:::

## applyDenoisePolicy widens; it never refuses

One function reconciles a mode with the settings it needs, and it has exactly
one production call site — the tracer's settings setter, which every path
funnels through:

{{cite ohao/render/rt/rt_meta.hpp "[[nodiscard]] constexpr RTRenderSettings applyDenoisePolicy(RTRenderSettings s) noexcept {"}}
{{cite ohao/render/rt/path_tracer.cpp "m_renderSettings = applyDenoisePolicy(settings);"}}

Both of its rules are monotone — they only flip a flag from false to true — which
is what makes it safe on every frame's settings push. It is also, on every input
the running engine can produce, a no-op: both stock profiles already set
`enableAuxiliaryAOVs` and `allowExternalDenoiser`.

What it does *not* do is reject. Capability is declared —

{{cite ohao/render/rt/rt_meta.hpp "[[nodiscard]] constexpr bool denoiseIsRealtimeCapable(DenoiseMode m) noexcept { return m != DenoiseMode::OIDN; }"}}

— but nothing consults it before running a mode: its only caller is an assertion
in the engine test suite. `isValidDenoiseMode` and `denoiseModeIndex` have no
callers, and guard a boundary that does not exist: no integer is ever cast into a
`DenoiseMode` anywhere.

## Contracts

- `applyDenoisePolicy` runs on every per-frame settings push, so it must stay monotone and idempotent. A rule that clears a flag would fight the profile defaults once per frame.
- Adding a `DenoiseMode` means touching `denoiseModeName`, `isValidDenoiseMode` and every predicate in `rt_meta.hpp` that should name it. Nothing in the build will tell you if you miss one.
- A temporal denoiser must be named in `tracerAccumulates`' non-accumulating arms, or the trace hands it a running mean that a moved view restarts.
- The tracer's mode is reset to the profile default every RT frame; a user's choice reaches it only through `applyRTRenderSettings`. Setting it once through `setRTRenderSettings` does not stick unless `setDenoiseMode` latched the override.
- `parseDenoiseMode` must never return a mode whose backend was compiled out; the denoise switch in `readback()` assumes the mode implies its resources were written.
