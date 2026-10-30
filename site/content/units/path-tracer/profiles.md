---
module: path-tracer
id: profiles
title: Profiles and meta
standard: v2
figures: [path-tracer-profiles-transport]
---

## Two constants, one integrator

OHAO ships one ray-tracing renderer with two personalities. `PathTracer` has no
subclass anywhere in the tree and no `#ifdef` picks a profile: the difference between
the two is a pair of `constexpr` aggregates in `rt_settings.hpp`.

{{cite ohao/render/rt/rt_settings.hpp "inline constexpr RTRenderSettings kRealtimeRTSettings{"}}

Both profiles run the same shaders, `rt/path_tracer.slang`. The realtime profile takes
two bounces, a firefly clamp at luminance 10, and a temporal denoiser: a moving view is
one sample a frame, and the denoiser keeps the history. The constant names NRD; the
renderer's defaults swap in DLSS Ray Reconstruction when the build has the DLSS SDK and
the device is NVIDIA's (below).

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::NRD,   // a moving view is one sample a frame: the denoiser keeps the history"}}

It also turns the radiance cache on: its paths end in the cache past their first
vertex (see the raygen and radiance cache units), so two bounces a frame carry the light
of every later one. The setting defaults to off, which is what the offline profile keeps.

{{cite ohao/render/rt/rt_settings.hpp ".radianceCache = true,"}}

The setting also sizes the cache, once, before the `PathTracer` is built: 2^21 entries
(`RadianceCache::kDefaultCapacity`, 72 MiB) with it on, one entry with it off. The
one-entry cache exists only because the shaders bind its buffers in both profiles; the
host records no cache work for it.

{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setRadianceCacheCapacity(m_settings.radianceCache ? render::RadianceCache::kDefaultCapacity : 1u);"}}

The offline profile takes 32 bounces (Russian roulette ends most paths long before), no
clamp, and OIDN as its default post-process. Its constant does not merely clear the
clamp flag, it also zeroes the threshold:

{{cite ohao/render/rt/rt_settings.hpp ".fireflyClampLuminance = 0.0f,"}}

The raygen gates the clamp on *both* the flag bit and a positive threshold, and applies
it once, to the whole path's radiance:

{{cite shaders/rt/path_tracer.slang "if ((flags & kFlagFireflyClamp) != 0u && pc.tuning.x > 0.0)"}}

The host adds a third condition. The clamp trades variance for bias, which only a
temporal denoiser fed one sample a frame needs; a running mean converges without it. So
the flag is set only when the trace does not accumulate:

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_renderSettings.enableFireflyClamp && !tracerAccumulates(m_renderSettings))"}}

Under the realtime profile with any denoiser but NRD, à-trous or DLSS-RR, the clamp is
therefore off, and a realtime view with no denoiser converges to the same image the
offline profile does at the same bounce count.

## The denoise policy is a handful of functions

`rt_meta.hpp` holds what each denoiser asks of the trace, as plain `constexpr`
functions of the mode or of the settings: CPU readback (OIDN), motion vectors (NRD,
à-trous, DLSS-RR), the diffuse/specular split (NRD), sub-pixel jitter (NRD, DLSS-RR),
and realtime capability (everything but OIDN). Beside them sit the RT techniques'
concepts, which document the shadow and GI techniques' virtual surfaces.

{{cite ohao/render/rt/rt_meta.hpp "[[nodiscard]] constexpr bool denoiseNeedsPixelJitter(DenoiseMode m) noexcept {"}}

Two functions decide what the trace keeps. `tracerAccumulates` says whether the trace
keeps its own running mean of the beauty. A temporal denoiser carries the history
itself, through camera moves by the motion vectors, and takes this frame's sample
alone; offline NRD denoises the mean of a still view's samples.

{{cite ohao/render/rt/rt_meta.hpp "case DenoiseMode::NRD: return !isRealtimeProfile(s.profile);"}}

`tracerAccumulatesAOVs` says whether NRD's five inputs are themselves running means,
which is the offline NRD case only. Both are read by the per-frame path, which turns
them into the `control.y` length of the running mean and an AOV-accumulation flag.

{{cite ohao/render/rt/path_tracer_render.cpp "if (tracerAccumulatesAOVs(m_renderSettings)) pc.control.x |= kPTFlagAccumulateAOVs;"}}

`applyDenoisePolicy` runs inside `PathTracer::setRenderSettings` and is allowed to
override the profile: motion vectors or a diffuse/specular split force auxiliary AOVs
on, and CPU readback raises `allowExternalDenoiser`.

{{cite ohao/render/rt/path_tracer.cpp "m_renderSettings = applyDenoisePolicy(settings);"}}

In practice it is a no-op on every shipping path. Both profile constants already set
`enableAuxiliaryAOVs = true`, and every settings object that reaches `PathTracer`
descends from one of the two, so the AOV branch cannot fire; the other branch writes
`allowExternalDenoiser`, which nothing reads. `denoiseIsRealtimeCapable` is exercised
only by the engine test suite: nothing in the engine refuses to run OIDN interactively,
the CPU readback simply makes it slow.

## One transport, and the mode that takes none

{{figure path-tracer-profiles-transport "Conceptual map of how RTRenderSettings fields reach the GPU: per-frame values packed into push constants, denoiseMode selecting host-side passes, and fields with no consumer. Groupings read from path_tracer_render.cpp and renderer.cpp; no measured numbers."}}

Everything per frame that reaches the shader travels in the 256-byte push-constant
block: the bounce limit in `params.w`, the clamp threshold in `tuning.x`, the flags in
`control.x`, and the two clock answers in `control.y` and `control.z`. That ceiling is
a compile-time constant asserted against the struct, not a device limit read back from
`VkPhysicalDeviceLimits`. Vulkan guarantees 128 bytes; 256 is a bet on RT-class
hardware that the code makes and never checks.

{{cite ohao/gpu/layout_meta.hpp "kPTPushConstantsBytes = 256"}}

`denoiseMode` takes neither a push constant nor a pipeline state of its own. It
selects host-side work — which of the à-trous, DLSS-RR, NRD or OIDN passes runs around
the trace — and reaches the shader only as derived values: the AOV-accumulation and
DLSS-RR flag bits, the length of the running mean, and whether the clamp is on.

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_renderSettings.denoiseMode == DenoiseMode::Atrous && m_atrousDenoiser) {"}}

The pipeline itself carries no profile state: no specialization constant is attached
to any stage, so `createRTPipeline` running before the profile's settings arrive (they
are pushed after `init`) costs nothing.

{{cite ohao/render/rt/rt_profile_renderer.hpp "m_pathTracer.setShaderSet(m_shaderSet);"}}

## A profile is an object

`RTRealtimeRenderer` and `RTOfflineRenderer` are thin finals over a shared base that
wraps a `PathTracer` and forwards over forty accessors. Both pass
`PathTracerShaderSet`'s default to that base, the named entry points of
`rt/path_tracer.slang` (the four stages and the cache's update raygen):

{{cite ohao/render/rt/rt_profile_renderer.hpp ": RTProfileRendererBase(kRealtimeRTSettings, PathTracerShaderSet{}) {}   // rt/path_tracer.slang"}}
{{cite ohao/render/rt/path_tracer.hpp "const char* closestHitEntry{"}}

The pipeline takes each stage's entry name from the set:

{{cite ohao/render/rt/path_tracer_pipeline.cpp "stages[0].pName = m_shaderSet.raygenEntry;"}}

The base leaves four pure virtuals to the finals: `getName`, `getProfile`,
`getDefaultSettings` and `resetsAccumulationOnViewChange`. The first three return
identity and constants, and `IRTRendererProfile::getDefaultSettings` has no callers.
The settings aggregate is not virtual: it is handed up through the base constructor.

{{cite ohao/render/rt/rt_profile_renderer.hpp "RTProfileRendererBase(RTRenderSettings settings, PathTracerShaderSet shaderSet)"}}

Only the fourth virtual changes behaviour, and what it decides is what a camera move
means.

{{cite ohao/render/rt/rt_profile_renderer.hpp "if (resetsAccumulationOnViewChange()) {"}}

Offline answers yes and hard-resets: sample index back to the render seed, both clocks
to zero, NRD's previous matrices to identity. Realtime answers no:

{{cite ohao/render/rt/rt_profile_renderer.hpp "bool resetsAccumulationOnViewChange() const override { return false; }"}}

and instead raises a per-frame flag. The next frame restarts the trace's running mean
and keeps the denoisers' history, which reprojects through the motion vectors; the
motion vectors are valid because last frame's view-projection is a rendered frame's.

{{cite ohao/render/rt/path_tracer_render.cpp "pc.control.z = m_hasPreviousView ? 1u : 0u;"}}

Keeping the denoiser's history alive across a camera move is what makes an interactive
viewport usable; throwing everything away is what makes a reference render correct.

A change to the traced scene takes the same path. After `recordUpdates`, a frame whose
TLAS was built or refit, or that uploaded instance data or lights, calls
`notifyViewChanged` on the active profile, so an offline render restarts on a moved
instance and a still scene keeps accumulating:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "rtRenderer->notifyViewChanged();"}}

## What the per-frame reset keeps

`VulkanRenderer` maps a render mode to its profile's settings with one `const` member
returning `std::optional`; an empty answer means the mode is not path traced. The
offline answer is the constant itself. The realtime answer is the constant with its
denoiser switched to DLSS-RR when the build defines `OHAO_DLSS_ENABLED` and the
device's vendor ID is NVIDIA's (0x10DE), else NRD as the constant has it:

{{cite ohao/gpu/vulkan/renderer.cpp "std::optional<RTRenderSettings> VulkanRenderer::rtDefaults(RenderMode mode) const {"}}
{{cite ohao/gpu/vulkan/renderer.cpp "if (props.vendorID == 0x10DE) s.denoiseMode = DenoiseMode::DLSSRR;   // NVIDIA"}}

A capture through the API names its own denoiser and asks for none unless it says
otherwise, so a path-traced capture is the raw estimate by default:

{{cite ohao/api/params.hpp "Denoiser denoise{Denoiser::none};"}}

Per frame, `prepareRTSceneForFrame` overwrites the renderer's settings with those
defaults before doing anything else, and `renderRTPipeline` calls it before it records
the trace:

{{cite ohao/gpu/vulkan/renderer.cpp "m_rtSettings = defaults;"}}

`applyRTRenderSettings` then re-injects the two overrides a caller can hold: a
`--denoise` mode, whatever it is (None and OIDN included), and a bounce limit
(`--bounces`, `render.capture {bounces}`).

{{cite ohao/gpu/vulkan/renderer.cpp "if (m_maxBouncesOverride) m_rtSettings.maxBounces = *m_maxBouncesOverride;"}}

Anything else set through `setRTRenderSettings` lasts until the next RT frame: it is
overwritten before any raygen reads it.

:::key
The profile constants are authoritative every frame, not just at startup. A knob that
is not one of the two overrides is not a setting; it is a value discarded before the
next dispatch reads it.
:::

## Contracts

- Both profile renderers are created lazily by `ensureRTRenderer` and released only in `VulkanRenderer::shutdown`. Switching modes at runtime leaves two full `PathTracer` instances resident, each owning its own screen-sized storage images (several of them RGBA32F), and `forEachRTRenderer` applies resizes and render seeds to both. Scene data is not duplicated: both read the one `RenderScene` and the one bindless table.
- `setRenderSettings` must be called between frames, never inside a command buffer: a change of DLSS render scale makes it wait on device idle and reallocate every render target.
- `preferAccumulation` and `allowExternalDenoiser` exist in the struct, but no code reads them to change behaviour — they appear only in the profile log line. Do not add logic that assumes they are already honoured. {{cite ohao/gpu/vulkan/renderer.cpp "externalDenoiser="}}
- The offline profile's freedom from firefly clamping holds three ways (flag clear, threshold zero, and a trace that accumulates). Restoring a nonzero threshold and the flag on the offline constant would still clamp nothing while it accumulates, and would clamp the moment a temporal denoiser is chosen.
- `radianceCache` is read twice: at init, to size the cache (a profile created with it off cannot turn it on later, its cache holds one entry), and every frame, to set `kFlagCache`. Both conditions must hold for a path to end in the cache.
- `tracerAccumulates` and the denoisers' history are coupled: a mode that returns false must carry its own temporal history (or show raw one-sample frames), because the trace keeps none.
