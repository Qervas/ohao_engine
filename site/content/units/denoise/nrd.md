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

Two producers write those inputs. The realtime raygen includes the 43-line
`nrd_frontend.glsl`, which the compose pass includes too and which holds both
directions of the YCoCg rotation but only the forward direction of the
hit-distance map. The reference path tracer, `path_tracer.slang`, is Slang and
cannot include GLSL, so it carries its own copies of the two packing functions.
Neither half of the demodulation lives in a shared file: the divide is in each
raygen and the matching multiply is in `nrd_compose.comp`.

{{figure denoise-nrd-aov-flow "Conceptual data flow with the real binding numbers and formats from path_tracer_render.cpp — not a captured render. The dashed band marks the region in which radiance is YCoCg; crossing that boundary in the wrong direction is what turns a denoiser bug into a hue shift."}}

## Demodulation, and why only the diffuse gets divided

Temporal filters smooth *lighting*, and lighting is smooth; albedo is not. Radiance
that still carries texture detail makes the filter fight the texture, so the
diffuse channel is divided by first-hit diffuse albedo before packing, with a floor
to survive metals, where diffuse albedo is zero. The reference tracer:

{{cite shaders/rt/path_tracer.slang "const float3 diffDemod = diffuseOut / max(diffAlbedo, float3(0.03));"}}
{{cite shaders/rt/path_tracer.slang "diffAlbedo = base.rho * (1.0 - base.metalness);"}}

The specular channel is divided by nothing in either producer; compose multiplies
it by binding 25 regardless. What binding 25 holds is where the two producers
differ. The realtime raygen's AOV was built as the complement of a specular chain
whose throughput is white for dielectrics and base-colour for metals — what its
`OHAO_CRUDE_GLOSSY` baseline sets at the first bounce and what every later specular
bounce multiplies in:

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput *= mix(vec3(1.0), bAlbedo, bMetallic);"}}
{{cite shaders/rt/pt_raygen_realtime.rgen "firstHitSpecColor  = mix(F0, vec3(1.0), metallic);"}}

Writing $\mathbf{a}$ for base colour and $m$ for metallic, the two lerps multiply
to a single application of the Fresnel colour at both endpoints of the slider:

$$\underbrace{\mathrm{lerp}(1,\mathbf{a},m)}_{\text{applied inside the path}}\times\underbrace{\mathrm{lerp}(F_0,1,m)}_{\text{applied in compose}}=\begin{cases}F_0 & m=0\\ \mathbf{a} & m=1\end{cases}$$

and because a metal's $F_0$ *is* its base colour, both branches equal $F_0$.

The reference tracer writes the OpenPBR base's directional albedo toward the viewer
minus its diffuse part:

{{cite shaders/rt/path_tracer.slang "specColor = max(a - diffAlbedo, float3(0.0));"}}

Its specular radiance comes from sampling the whole BSDF, Fresnel included, so the
NRD composite of the reference applies the specular albedo twice: once in the path
and once in compose. The reference's own image (the running mean, below and in the
OIDN unit) is unaffected; only the NRD composite is.

## Where the lerp pair stops being exact

In the realtime raygen the identity holds only where the chain's throughput is that
lerp. It does not hold for the bounce-0 direct lobe. The
analytic NEE at the primary hit runs with throughput 1 and folds a full Schlick $F$
into the BRDF before adding to `specContrib`:

{{cite shaders/rt/pt_raygen_realtime.rgen "vec3 neeSpecular = neeCommon * spec;"}}

For metals that is still one application: $F\approx F_0=\mathbf{a}$ inside the ray,
$\times 1$ in compose. For dielectrics it is two — the radiance already carries $F$,
and compose multiplies by $F_0\approx 0.04$ a second time. Its live first-bounce
branch, a GGX VNDF sample, also sets `specThroughput` to true Fresnel rather than
$\mathrm{lerp}(1,\mathbf{a},m)$, so its *indirect* dielectric specular is
double-counted as well.

{{cite shaders/rt/pt_raygen_realtime.rgen "specThroughput = F * smithG2overG1GGX(NdotV, NdotL, alpha);"}}

The reference tracer splits differently: everything its primary hit gathers (the
light and environment samples at depth 0, whichever lobe reflects them) goes to the
diffuse channel and is divided by diffuse albedo, and every later vertex goes to the
channel of the lobe the first bounce sampled:

{{cite shaders/rt/path_tracer.slang "if (firstLobe == 1u) diffuseOut += add;"}}

The comment on the realtime specColor line records what the lerp defends against:

{{cite shaders/rt/pt_raygen_realtime.rgen "double-darkened metals to near-black in NRD"}}

:::why
The lerp pair was written to fix metals, not dielectrics. The rejected alternative
is the obvious one — `specColor = F0`, no lerp to white — and because a metal's
$F_0$ is its base colour, that multiplied base colour into radiance that already
carried it, driving metals to near-black. Lerping the AOV to white at $m=1$ removes
the second application and costs no extra AOV; the dielectric bounce-0 case above
is the part of the bill still outstanding.
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
triple through the inverse basis, which its header warns about:

{{cite shaders/rt/nrd_compose.comp "REBLUR OUT is YCoCg (NRD.hlsli Pack/Unpack)"}}

Pack correctly but skip demodulation, and the input is in the right basis but the
wrong space — the filter's cross-channel behaviour drifts on albedo-coloured
radiance, and compose applies surface colour a second time: a hue shift plus a
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

Nothing reads $z$ back from binding 20. Each raygen computes it at the primary hit,
keeps it in a local, and writes it to that binding for NRD's separate `IN_VIEWZ`
slot; the normalization uses the local:

{{cite shaders/rt/pt_raygen_realtime.rgen "firstHitViewZ = -viewPos.z;"}}
{{cite shaders/rt/path_tracer.slang "viewZ = dot(hit.position - camPos, fwd);"}}

The constants are NRD's `ReblurHitDistanceParameters` defaults, hardcoded in GLSL
and again, as literals, in the Slang copy:

{{cite shaders/includes/rt/nrd_frontend.glsl "const vec3 NRD_HIT_DIST_PARAMS = vec3(3.0, 0.1, 20.0);"}}
{{cite shaders/rt/path_tracer.slang "const float f = (3.0 + abs(viewZ) * 0.1) * lerp(20.0, 1.0, smc);"}}

$s$ is where OHAO knowingly diverges. NRD derives it from a richer function of
roughness; both producers substitute $\text{roughness}^2$:

{{cite shaders/includes/rt/nrd_frontend.glsl "float smc = roughness * roughness; // lobe-spread proxy"}}

The producers also *measure* $d$ differently. The realtime raygen's diffuse records
only the first secondary hit — one segment, the local occluder — while its specular
sums the whole chain while it stays specular, so a mirror reports the distance to
the virtual image rather than to the mirror:

{{cite shaders/rt/pt_raygen_realtime.rgen "specHitDist += payload.hitDist;"}}

The reference tracer records one segment for both: the length of the first bounce
ray, given to the channel of the lobe that drew it, and 0 to the other.

{{cite shaders/rt/path_tracer.slang "if (depth == 1u) secondHitT = (hit.flags & kHit) != 0u ? hit.t : 0.0;"}}

The sharp edge is that $A$ exists three times. Both shaders hardcode it at 3.0; the
C++ profile re-declares it and pushes it into `ReblurSettings` — while $B$ and $C$
are left at NRD's defaults and have no C++ knob at all:

{{cite ohao/render/rt/denoise/nrd_denoise.cpp "s.hitDistanceParameters.A         = p.hitDistanceParamA;"}}

Change `hitDistanceParamA` and the halves normalize against different scales, with
no build-time check that they agree.

## Which raygen feeds NRD

Each profile renderer names its own shaders. The realtime one passes the GLSL
raygen explicitly; the offline one takes `PathTracerShaderSet`'s default, the Slang
reference:

{{cite ohao/render/rt/rt_profile_renderer.hpp "bin/shaders/rt_pt_raygen_realtime.rgen.spv"}}
{{cite ohao/render/rt/path_tracer.hpp "const char* raygenSpv{"bin/shaders/rt_path_tracer.slang.raygen.spv"};"}}

A second gate matters before profiling: REBLUR, compose and the cinematic chain run
only in NRD mode, on top of the denoiser existing and auxiliary AOVs being on:

{{cite ohao/render/rt/path_tracer_render.cpp "if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs && m_renderSettings.denoiseMode == DenoiseMode::NRD) {"}}

Each piece of the chain writes its descriptor sets at record time,
so each keeps one set per frame in flight, selected by the frame slot the tracer
passes in.

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
independent of the path tracer's RT layout: five storage images, no push
constants, an 8×8 workgroup. The whole shader is an unpack and a remodulate:

{{cite shaders/rt/nrd_compose.comp "vec3 composed = diffRad * diffAlbedo + specRad * specColor;"}}

Alpha is dropped here. NRD's own input contract defines the normalized hit distance
as ambient occlusion on the diffuse channel and specular occlusion on the specular
one, and says the `.w` of REBLUR's output carries the filtered version — a signal an
application can spend on second-bounce diffuse or on damping IBL. OHAO spends it on
nothing: compose reads `.rgb` and discards `.w`, so here it really is only a control
signal for REBLUR's own filter widths.

## Tuning for a path tracer instead of a game

REBLUR ships with a 30-frame linear accumulation window. OHAO feeds it AOVs that
are already averaged over N samples, so the profile pushes the window to REBLUR's
documented ceiling of 63 instead — the code states both numbers where it makes the
change:

{{cite ohao/render/rt/denoise/nrd_denoise.hpp "NRD default 30; we bump to 63"}}

The "multi-sample" part is a running mean written by the raygen itself: in
accumulate mode each AOV binding is blended with its previous value at weight
$1/(n{+}1)$, so after $N$ samples the image is intended to hold their mean rather
than the last sample. The realtime raygen does it in a macro, the reference tracer
with a `lerp` whose weight is 1 on the first sample:

{{cite shaders/rt/pt_raygen_realtime.rgen "_ohaoAovCur = mix(_ohaoAovPrev, _ohaoAovCur, 1.0 / _ohaoAovN);"}}
{{cite shaders/rt/path_tracer.slang "const bool blendAOVs = accumulateAOVs && pc.params.z > 0u;"}}

That is an `imageLoad` of the previous dispatch's contents, and the mean is only as
good as those contents. Every `render()` re-records the AOV transition block with
`oldLayout = VK_IMAGE_LAYOUT_UNDEFINED`, which the spec explicitly permits an
implementation to treat as a discard — the running average is a property the code
relies on rather than one Vulkan guarantees:

{{cite ohao/render/rt/path_tracer_render.cpp "aovBarriers[5].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;"}}

The other half of the temporal story is throwing history away. On any frame where
the view changed, the host reports frame index 0 *and* overwrites the previous
view/projection matrices with the current ones, so NRD cannot reproject from a
stale pose and trail a ghost behind a moving camera:

{{cite ohao/render/rt/path_tracer_render.cpp "camera.frameIndex = m_viewChangedThisFrame ? 0u : m_historyFrameCount;"}}

That bootstrap costs one spatial-only frame per camera move — the trade the code
takes over smeared geometry.

## The cinematic tail

`NrdCinematicPost` consumes the composed HDR at binding 29 and runs bloom extract,
two downsample-blur passes, a tonemap composite, then a depth-of-field gather.
Two things in here read as one thing and are another. First, the tonemap:
`cinematic_composite.comp` still carries a complete AgX implementation — inset and
outset matrices, the 6th-order contrast fit — but `main()` calls none of it and
applies Khronos PBR Neutral instead:

{{cite shaders/rt/cinematic_composite.comp "Tonemap — Khronos PBR Neutral"}}

Second, a naming hazard that survives an earlier revision: the input struct's
`tonemappedOut` field no longer carries the final image. Since DoF was added the
composite writes the *pre*-DoF image at binding 32, and only the gather writes
binding 30 — which is what the host reads back.

{{cite ohao/render/rt/denoise/nrd_cinematic.hpp "is wired to the pre-DoF LDR image"}}

## Contracts

- Bindings 22/23 are YCoCg + normalized hit distance, and 27/28 come back the same way. A consumer that reads them as linear RGB — a debug visualizer, say — shows scrambled hue, not an obvious error.
- `NRD_HIT_DIST_PARAMS.x` (3.0, in GLSL), the 3.0 literal in `path_tracer.slang`'s pack and `NrdReblurProfile::hitDistanceParamA` must agree. Changing one desynchronizes the normalization silently; nothing checks.
- `setCommonSettings()` must run before every `denoise()` — it carries the frame index and the previous view/projection NRD reprojects from. `setInputResources()` only stashes handles into the impl and they persist across frames, so the per-frame call is redundant; what breaks is failing to re-stash after any AOV image is recreated, since `denoise()` forwards whatever is there with no validation.
- The realtime raygen's `specColor` (binding 25) is `lerp(F0, 1, metallic)`, not `F0`. The `1` at the metal end is what stops metals being darkened twice; bounce-0 direct specular and the live VNDF indirect chain already carry Schlick `F`, so dielectrics get it twice. The reference tracer's binding 25 is its specular albedo, and its specular radiance carries Fresnel too: its NRD composite applies specular twice.
- The multi-sample AOV mean assumes the previous dispatch's image contents survive an `UNDEFINED`→`GENERAL` transition. Nothing in the API promises that; a driver that discards turns the mean into the last sample.
- The REBLUR + compose + cinematic chain runs only when the denoiser exists, auxiliary AOVs are on and the denoise mode is NRD. The raygen still writes the AOVs whenever they are on.
