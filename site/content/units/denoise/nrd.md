---
module: denoise
id: nrd
title: NRD REBLUR + compose
standard: v2
figures: [denoise-nrd-aov-flow]
---

## Three encodings REBLUR insists on

NVIDIA's REBLUR is a spatio-temporal accumulator that reads meaning out of the
bits you hand it, and it refuses almost everything a path tracer naturally
produces. Its diffuse and specular inputs must be *demodulated* (surface colour
divided out), carried in *YCoCg* rather than linear RGB, and their alpha channel
must hold a hit distance already *normalized* into [0,1] by NRD's own
view-depth-and-roughness formula. Get either of the first two wrong and the failure
is not subtle noise — it is a hue shift; get the third wrong and REBLUR quietly
filters at the wrong radius.

One producer writes those inputs: `path_tracer.slang`, which both RT profiles run.
It is Slang and packs inline. The consumer side is GLSL: `nrd_compose.comp`
includes `nrd_frontend.glsl`, which holds both directions of the YCoCg rotation
and the forward hit-distance map, though compose calls only the inverse rotation.
Neither half of the demodulation lives in a shared file: the divide is in the
raygen and the matching multiply is in `nrd_compose.comp`.

{{figure denoise-nrd-aov-flow "Conceptual data flow with the real binding numbers and formats from path_tracer_render.cpp — not a captured render. The dashed band marks the region in which radiance is YCoCg; crossing that boundary in the wrong direction is what turns a denoiser bug into a hue shift."}}

NRD is the realtime profile's denoiser wherever DLSS Ray Reconstruction cannot run
(a build without the SDK, or a GPU not NVIDIA's), and the offline profile runs it when
asked:

{{cite ohao/render/rt/rt_settings.hpp ".denoiseMode = DenoiseMode::NRD,   // a moving view is one sample a frame: the denoiser keeps the history"}}

## Demodulation, and why only the diffuse gets divided

Temporal filters smooth *lighting*, and lighting is smooth; albedo is not. Radiance
that still carries texture detail makes the filter fight the texture, so the
diffuse channel is divided by first-hit diffuse albedo before packing, with a floor
to survive metals, where diffuse albedo is zero:

{{cite shaders/rt/path_tracer.slang "const float3 diffDemod = diffuseOut / max(diffAlbedo, float3(0.03));"}}
{{cite shaders/rt/path_tracer.slang "diffAlbedo = base.rho * (1.0 - base.metalness);"}}

The specular channel is divided by nothing, and compose multiplies it by nothing:
its radiance comes from sampling the whole BSDF, Fresnel included, so it already is
the reflected light. Binding 25 still carries the specular albedo toward the viewer
— the OpenPBR base's directional albedo minus its diffuse part — as a guide for DLSS
Ray Reconstruction; compose binds it and does not read it.

{{cite shaders/rt/path_tracer.slang "specColor = max(a - diffAlbedo, float3(0.0));"}}

What the camera ray itself meets and that emits — a light's emitter mesh, an
emissive surface, the sky — is in neither channel. It has no albedo to divide out
and nothing to filter, so the raygen writes it to its own guide at binding 13, and
compose adds it after REBLUR:

{{cite shaders/rt/path_tracer.slang "emissionAOV[pixel] = lerp(emissionAOV[pixel], float4(add, 1.0), blendAOVs ? 1.0 / float(pc.control.y + 1u) : 1.0);"}}

## How the lobes are split

At the primary hit the split is by lobe evaluation. The light sample and the
environment sample each evaluate the BSDF as its two parts, specular and diffuse,
and each part goes to its own channel:

{{cite shaders/rt/path_tracer.slang "if (depth == 0u) { diffuseOut += throughput * fd * ls.radiance * w; specularOut += throughput * fs * ls.radiance * w; }"}}

Every later vertex goes to the channel of the lobe the first BSDF sample drew:

{{cite shaders/rt/path_tracer.slang "if (firstLobe == 1u) diffuseOut += add;"}}

So a direct highlight travels in the specular channel, filtered by REBLUR's
specular path at the surface's roughness, and indirect light follows the lobe that
carried the path off the surface.

:::why
One path a sample keeps the beauty a single unbiased estimate: the channels
partition what the path adds, they do not trace twice. Past the first vertex the
partition is coarse — a channel is chosen once per path, by its first bounce — but
direct light, most of most images, is split exactly.
:::

## YCoCg, and the two ways to get magenta

The forward transform is the standard reversible luma/chroma rotation ported from
`NRD.hlsli`, where $Y$ is luma and $C_o,C_g$ are the orange and green chroma axes:

$$Y=\tfrac14 R+\tfrac12 G+\tfrac14 B,\qquad C_o=\tfrac12(R-B),\qquad C_g=-\tfrac14 R+\tfrac12 G-\tfrac14 B$$

The reference tracer writes the same three dot products inline:

{{cite shaders/rt/path_tracer.slang "return float4(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5))"}}

The inverse is done without a matrix: $t=Y-C_g$ recovers $(R+B)/2$, so
$G=Y+C_g$, $R=t+C_o$, $B=t-C_o$. Algebraically exact — but the unpack still
clamps, because a spatio-temporal filter reconstructs a value that was never in
the original gamut and can land slightly negative:

{{cite shaders/includes/rt/nrd_frontend.glsl "return max(r, vec3(0.0));"}}

Two distinct mistakes produce the same magenta symptom. Skip the pack, and REBLUR
reads red as luma and green/blue as chroma; compose then rotates an already-linear
triple through the inverse basis, which its unpack names:

{{cite shaders/rt/nrd_compose.comp "Back-end unpack: REBLUR writes YCoCg in .rgb"}}

Pack correctly but skip demodulation, and the input is in the right basis but the
wrong space — the filter's cross-channel behaviour drifts on albedo-coloured
radiance, and compose applies the diffuse albedo a second time: a hue shift plus a
visible darkening.

:::key
Magenta in the NRD path is almost never a denoiser bug. It means a producer and a
consumer disagree about which colour space binding 22/23 is in. Check the pack
before you touch anything in NRD.
:::

## The hit distance is a control signal, not data

REBLUR uses alpha to decide how hard to filter each pixel, so the world-space ray
length has to be mapped into [0,1] against a scene-scale reference. With $d$ the
ray length, $z$ the first hit's view-space depth and $s\in[0,1]$ a lobe-spread
term:

$$f=(A+|z|B)\cdot\mathrm{lerp}(C,1,s),\qquad \hat d=\mathrm{clamp}\!\left(\tfrac{d}{f},0,1\right)$$

Nothing reads $z$ back from binding 20. The raygen computes it at the primary hit,
keeps it in a local, and writes it to that binding for NRD's separate `IN_VIEWZ`
slot; the normalization uses the local:

{{cite shaders/rt/path_tracer.slang "viewZ = dot(hit.position - camPos, fwd);"}}

The constants are NRD's `ReblurHitDistanceParameters` defaults, as literals in the
raygen's pack and again in the GLSL header:

{{cite shaders/rt/path_tracer.slang "const float f = (3.0 + abs(viewZ) * 0.1) * lerp(20.0, 1.0, smc);"}}
{{cite shaders/includes/rt/nrd_frontend.glsl "const vec3 NRD_HIT_DIST_PARAMS = vec3(3.0, 0.1, 20.0);"}}

$s$ is where OHAO knowingly diverges. NRD derives it from a richer function of
roughness; the raygen substitutes $\text{roughness}^2$:

{{cite shaders/rt/path_tracer.slang "const float smc = saturate(roughness * roughness);"}}

$d$ is one segment: the length of the first bounce ray, given to the channel of
the lobe that drew it, and 0 to the other. A mirror therefore reports the distance
to the mirror's first reflected hit, not the length of a chain of reflections.

{{cite shaders/rt/path_tracer.slang "if (depth == 1u) secondHitT = (hit.flags & kHit) != 0u ? hit.t : 0.0;"}}

The sharp edge is that $A$ exists three times. The raygen hardcodes it at 3.0, the
GLSL header at 3.0, and the C++ profile re-declares it and pushes it into
`ReblurSettings` — while $B$ and $C$ are left at NRD's defaults and have no C++
knob at all:

{{cite ohao/render/rt/denoise/nrd_denoise.cpp "s.hitDistanceParameters.A         = p.hitDistanceParamA;"}}

Change `hitDistanceParamA` and the halves normalize against different scales, with
no build-time check that they agree.

## When the chain runs

REBLUR and compose run only in NRD mode, on top of the denoiser existing and
auxiliary AOVs being on:

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs && m_renderSettings.denoiseMode == DenoiseMode::NRD) {"}}

Both write their descriptor sets at record time, so each keeps one set per frame
in flight, selected by the frame slot the tracer passes in.

## Lending NRD a device it did not create

NRD's Vulkan path goes through NRI, which normally creates its own device. Here it
is handed OHAO's, described by a `DeviceCreationVKDesc` carrying the extension lists
the app passed to `vkCreateInstance` and `vkCreateDevice`. The instance list is
copied verbatim:

{{cite ohao/render/rt/denoise/nrd_denoise.cpp "instanceExtensionsStorage.assign(instanceExtensions.begin(), instanceExtensions.end());"}}

The device list loses three. NRI's dispatch-table resolver eagerly looks up
`vkCmdTraceRaysIndirect2KHR` whenever it sees `VK_KHR_ray_tracing_pipeline`, and
that entry point lives in `VK_KHR_ray_tracing_maintenance1`, which OHAO does not
enable. REBLUR is compute-only, so acceleration-structure, RT-pipeline and
deferred-host-operations names are filtered out of the NRI-facing copy:

{{cite ohao/render/rt/denoise/nrd_denoise.cpp "std::strcmp(e, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0 ||"}}

Resources cross as raw `VkImage` plus `VkFormat`, not image views — NRI builds its
own views from the format. All seven slots are declared as already being in
`SHADER_RESOURCE_STORAGE` (`VK_IMAGE_LAYOUT_GENERAL`). For the five inputs that is
where the raygen leaves them. The two `OUT_` images are written by no shader before
this point; the host puts them in `GENERAL` with an explicit `UNDEFINED`→`GENERAL`
barrier recorded ahead of the trace, in the same block that transitions the AOVs:

{{cite ohao/render/rt/path_tracer_render.cpp "aovBarriers[10].image = m_outDiffRadianceImage;"}}

The snapshot then asks for the layouts to be restored afterwards so compose and the
host readbacks find what they expect:

{{cite ohao/render/rt/denoise/nrd_denoise.cpp "snapshot.restoreInitialState = true;"}}

## Compose is deliberately trivial

`NrdCompositor` owns a standalone compute pipeline whose descriptor set layout is
independent of the path tracer's RT layout: seven storage images, no push
constants, an 8×8 workgroup. The shader unpacks, remodulates the diffuse, adds the
specular and the camera ray's emission:

{{cite shaders/rt/nrd_compose.comp "vec3 composed = diffRad * diffAlbedo + specRad + emission;"}}

It writes that sum twice: as HDR, and as the frame's output (binding 6, the tracer's
RGBA8 image) with the tracer's own curve, ACES at 0.5 and 2.2 gamma. A window
therefore presents the denoised frame, and a capture under NRD reads the same image
any other mode leaves there:

{{cite shaders/rt/nrd_compose.comp "imageStore(outLdr, p, vec4(pow(aces(composed * 0.5), vec3(1.0 / 2.2)), 1.0));"}}

Alpha is dropped. NRD's own input contract defines the normalized hit distance
as ambient occlusion on the diffuse channel and specular occlusion on the specular
one, and says the `.w` of REBLUR's output carries the filtered version — a signal an
application can spend on second-bounce diffuse or on damping IBL. OHAO spends it on
nothing: compose reads `.rgb` and discards `.w`, so here it really is only a control
signal for REBLUR's own filter widths.

## Tuning for a path tracer instead of a game

REBLUR ships with a 30-frame linear accumulation window. The profile pushes the
window to REBLUR's documented ceiling of 63 instead — the code states both numbers
where it makes the change:

{{cite ohao/render/rt/denoise/nrd_denoise.hpp "NRD default 30; we bump to 63"}}

What NRD is fed depends on the profile. Realtime NRD takes this frame's sample
alone: the trace keeps no running mean (`tracerAccumulates` is false) and REBLUR
carries the whole history. Offline NRD denoises the mean of a still view's
samples: the five guide images become a running mean too, each blended with its
previous value at weight $1/(n{+}1)$, where $n$ is the frames already in the mean:

{{cite shaders/rt/path_tracer.slang "const bool blendAOVs = accumulateAOVs && pc.control.y > 0u;"}}
{{cite ohao/render/rt/rt_meta.hpp "return s.denoiseMode == DenoiseMode::NRD && !isRealtimeProfile(s.profile);"}}

That blend is an image load of the previous dispatch's contents, and the mean is
only as good as those contents. Every `render()` re-records the AOV transition
block with `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED`, which the spec explicitly
permits an implementation to treat as a discard — the running average is a
property the code relies on rather than one Vulkan guarantees:

{{cite ohao/render/rt/path_tracer_render.cpp "aovBarriers[5].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;"}}

The other half of the temporal story is when history is thrown away. The frame
index handed to NRD is the denoisers' clock, which only `resetAccumulation`
restarts; a moved view in the realtime profile restarts nothing, and NRD
reprojects through the motion vectors and last frame's view and projection, which
the host captures every frame:

{{cite ohao/render/rt/path_tracer_render.cpp "camera.frameIndex = m_historyFrameCount;   // 0 only after resetAccumulation: a moved view reprojects"}}
{{cite ohao/render/rt/path_tracer_render.cpp "m_prevViewMatrix = viewM;"}}

The offline profile resets accumulation on a view change, so offline NRD starts
from frame 0 after every move.

## Contracts

- Bindings 22/23 are YCoCg + normalized hit distance, and 27/28 come back the same way. A consumer that reads them as linear RGB — a debug visualizer, say — shows scrambled hue, not an obvious error.
- The 3.0 literal in `path_tracer.slang`'s pack, `NRD_HIT_DIST_PARAMS.x` in `nrd_frontend.glsl` and `NrdReblurProfile::hitDistanceParamA` must agree. Changing one desynchronizes the normalization silently; nothing checks.
- `setCommonSettings()` must run before every `denoise()` — it carries the frame index and the previous view/projection NRD reprojects from. `setInputResources()` only stashes handles into the impl and they persist across frames, so the per-frame call is redundant; what breaks is failing to re-stash after any AOV image is recreated, since `denoise()` forwards whatever is there with no validation.
- The specular channel already carries Fresnel and is never multiplied by binding 25; binding 25 is a guide for DLSS only. The camera ray's emission (binding 13) bypasses REBLUR and is added in compose: any new emissive term at the primary hit belongs there, not in a channel.
- Compose writes the frame's output with the tracer's curve; a change to that curve must change the raygen, `nrd_compose.comp` and `dlss_tonemap.comp` together.
- Offline NRD's AOV mean assumes the previous dispatch's image contents survive an `UNDEFINED`→`GENERAL` transition. Nothing in the API promises that; a driver that discards turns the mean into the last sample.
- NRD's frame index must stay the denoisers' clock, not the trace's running-mean count: a moved view restarts the mean but must not restart REBLUR's history.
- REBLUR and compose run only when the denoiser exists, auxiliary AOVs are on and the denoise mode is NRD. The raygen still writes the AOVs whenever they are on.
