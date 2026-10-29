---
module: hybrid
id: gi-technique
title: RT GI technique
standard: v2
---

## The bounce the ambient term cannot know

Without RT GI the deferred pipeline's indirect diffuse light is a guess: the
environment's irradiance when one is bound, and with none a constant, one push-constant
scalar times the surface's diffuse directional albedo (none for a metal) times AO (the baked AO, and SSAO when
it is on). Neither can know that a red wall stands beside a white floor, so the floor
stays white. `RTGITechnique` replaces that guess with traced rays, one bounce deep:
light that leaves a lit surface, lands on another and is reflected once more toward the
camera. With it the constant is not applied at all — the walls are lit by direct light
and GI's rays only, as in the path tracer, which has no such term:

{{cite shaders/core/deferred_lighting.slang "else if (!ibl) ambient += pc.ambientIntensity * diffuseAlbedo * ao * ssao;"}}

On the helmet room that took the helmet against its left wall to 0.534 (the path
tracer's 0.534) and the wall itself from 0.645 to 0.523 (the path tracer: 0.610, with
every bounce, where RT GI traces one).

The result is a full-resolution `RGBA16F` image the lighting pass reads at binding 11:
the incoming radiance over the cosine lobe in rgb (the receiver's albedo not applied)
and the environment's visibility in alpha, the share of the rays that left the scene:

{{cite ohao/render/rt/render_technique.hpp "VkImage indirectLight;         // RGBA16F, full resolution: rgb incoming radiance (no albedo), a the environment's visibility"}}

## Two compute passes, no ray tracing pipeline

The technique is two compute passes, timed under their registry names: the trace,
`rt.gi`, and in realtime quality the denoiser, `rt.gi.denoise`. Neither is a ray
tracing pipeline. The trace uses inline ray queries — no shader binding table, no
raygen, miss or hit shaders — and without `VK_KHR_ray_query` `init()` is never
called and the frame has no indirect light.

{{cite ohao/render/rt/rt_gi_technique.hpp "// light for the lighting pass (binding 11), traced with inline ray queries"}}

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_rayQuery && m_rtGI->init(device, physicalDevice, m_width, m_height)) {"}}

Both are Slang: `trace` in `rt/gi_trace.slang`, and three entry points in
`rt/gi_denoise.slang` — `temporal`, `atrous` and `upsample` — each with its own
binding numbers and descriptor set layout. All four load into plain compute
pipelines through the helper RT GI shares with RT shadows in `rt_compute.cpp`:

{{cite ohao/render/rt/rt_gi_technique.cpp "return rtc::createComputePipeline(m_device, "rt_gi_trace.slang.spv", "trace", traceSets,"}}

## The estimator, and where the albedo goes

For a Lambertian receiver of albedo $\rho$, outgoing radiance is

$$L_o = \frac{\rho}{\pi}\int_{\Omega} L_i(\omega)\,\cos\theta \; d\omega$$

Drawing directions with density $p(\omega) = \cos\theta/\pi$ — a cosine-weighted
hemisphere around the shading normal — makes the Monte-Carlo weight collapse:

$$\hat{L}_o = \frac{1}{N}\sum_{k=1}^{N} \frac{(\rho/\pi)\,L_i(\omega_k)\cos\theta_k}{\cos\theta_k/\pi} \;=\; \rho \cdot \frac{1}{N}\sum_{k=1}^{N} L_i(\omega_k)$$

with $N$ the ray count, $\omega_k$ the $k$-th sampled direction and $\theta_k$ its
angle to the normal. That cancellation is why the receiver's $\pi$ and its cosine
appear nowhere in the trace: it averages the radiance its rays bring back and writes
that mean, the irradiance over $\pi$, without multiplying by anything, beside the share
of its rays that escaped:

{{cite shaders/rt/gi_trace.slang "const float4 mean = float4(sum / float(rays), escapedSum / float(rays));"}}

The directions are stratified in azimuth: ray $s$ of $N$ takes its angle from the
$s$-th of $N$ equal sectors, jittered within it, and its radius from $\sqrt{u}$,
which is what makes the projected disk sample cosine-distributed. With realtime's
two rays a pixel, the pair always lands in opposite halves of the hemisphere.

{{cite shaders/rt/gi_trace.slang "const float phi = kTwoPi * (float(s) + rand01(key, s, 1u)) / float(rays);"}}

$\rho$ is applied at the very end, at full resolution, by the lighting pass, and it is
the *diffuse* directional albedo toward the viewer: OpenPBR's EON diffuse under the
specular layer, scaled by what the specular lets through, for the G-buffer's base colour
and metalness (`OpenPBRBase::diffuseAlbedo`). A fully metallic surface has none, so it
receives no diffuse bounce at all. The radiance itself still reaches it: the
specular IBL reads it for what the reflection sees where the environment is occluded.

{{cite shaders/core/deferred_lighting.slang "const float3 diffuseAlbedo = s.bsdf.diffuseAlbedo(s.wo, tables);"}}

{{cite shaders/core/deferred_lighting.slang "if (rtGI) ambient += gi.rgb * diffuseAlbedo * ao;"}}

Keeping albedo out of the half-resolution chain is deliberate. Everything the
denoiser averages is incoming light, which varies smoothly across a surface; the
albedo, which carries texture detail, never passes through a blur or a half-resolution
sample.

## What a ray brings back

A ray that hits a surface returns what that surface reflects over $\pi$ times the
direct light the hit point receives — its own one-bounce $L_i$ — plus what it emits:

{{cite shaders/rt/gi_trace.slang "return (albedo * (1.0 / kPi) * directAt(at, -dir, key, s) + hm.emission) * T;"}}

What it reflects is the hit's own material. `hitMaterial` (`rt/alpha_test.slang`) finds
the hit triangle's material through the instance's material refs and resolves it at the
hit's uv as the G-buffer and the path tracer's closest hit do: base colour and its
texture, roughness and metalness and their texture, the specular weight and colour and
their textures, and the emission:

{{cite shaders/rt/alpha_test.slang "HitMaterial hitMaterial(uint slot, uint primitive, float2 bary)"}}

The trace then takes that material's OpenPBR directional albedo, the whole base's
(specular and diffuse together), at normal incidence — the hit's normal taken as facing
the ray — through OpenPBR's energy tables, which the technique binds at 11:

{{cite shaders/rt/gi_trace.slang "const float3 albedo = OpenPBRBase(hm.inputs).albedo(float3(0.0, 0.0, 1.0), tables);"}}

{{cite ohao/render/rt/rt_gi_technique.cpp "if (!m_openpbrTables.create(device, physicalDevice)) {"}}

The direct light at the hit comes from every light of the frame. The CPU writes them every
frame from the `RenderScene`'s light list — in scene order, emissive proxies and dark lights
out — into this frame slot's light buffer, which grows to the count, each with its share of
the lights' power (the luminance of colour times intensity) in its fourth radiance component
and its slot of Vose's alias table in the spot cone's two spare components:

{{cite ohao/render/rt/rt_gi_technique.cpp "lights[i].radiance[3] = static_cast<float>(power[i] / total);"}}

Each is attenuated as the lighting pass attenuates it: inverse square, windowed by
the light's range, and a spot light's cone. A rectangle light is what it is from afar:
a point at its centre of intensity `Le * area * cos(theta)` toward its face, a spot of
cones 0 and 90 degrees (whose linear ramp is exactly the cosine), in the path tracer's
units and with no range; a directional light is a direction, not a position.

{{cite shaders/rt/gi_trace.slang "atten = window * window / max(d2, 0.01);"}}

{{cite ohao/render/rt/rt_gi_technique.cpp "g.positionAndType = f4(d.position + 0.5f * (d.areaEdge1 + d.areaEdge2), 2.0f);"}}

The hit is shadowed with one ray. Each light would give the hit some luminance unshadowed;
one is chosen with probability proportional to it, a shadow ray goes to it, and its light
is divided by that probability. The mean is the sum of every light's shadowed light, for
one ray where n would cost n. Up to `kGIWeighedLights` (16) lights, each is weighed, in one
pass: a weighted reservoir, one random number rescaled at every light. Past that, weighing
all of them at every hit costs too much (the benchmark scenes have 1,000), so
`kGILightCandidates` (eight) candidates are drawn by power from the alias table, in constant
time each, and one is chosen among them by what it gives. That is resampled importance sampling: the
candidate's weight is what it gives over its probability, and the estimate, scaled by the
weights' mean, is unbiased for any number of lights:

{{cite shaders/rt/gi_trace.slang "scale = sum / (float(kGILightCandidates) * luminance(lit));"}}

Until 2026-10-29 the bounce saw only the four brightest lights. Cornell has twelve of equal
intensity, so its walls bounced a third of their light. `check_analytic.py` now holds it:
a spot aimed at a wall lights the floor only by the bounce, and 6, 12 or 20 spots at one
place must bounce what one spot of their sum does, and twenty spread spots of twenty
strengths what the twenty do alone, at reference quality (the realtime denoiser's clamp is
not linear).

Before it, a bounce point in shadow bounced full light, and every surface facing a
shadowed floor, an underside or a rim, glowed with light that was not there. One
shortcut is left: the hit has no normal of its own — the trace has only the hit distance and the
instance, not the triangle's shading normal — so it takes the negated ray direction,
which is exact only for a ray striking the surface head-on.

A ray that commits nothing counts as escaped and samples the environment map in its
direction, through the same equirect mapping the lighting pass uses, times
`Environment.intensity` as the path tracer's miss applies it, or brings back black when
none is bound:

{{cite shaders/rt/gi_trace.slang "return envMap.SampleLevel(envSampler, uv, 0.0).rgb * pc.cameraPos.w * T;   // times Environment.intensity"}}

The sample is level 0 of the source image, which has a single mip level, so a miss reads
full-resolution texels and a bright spot in the HDRI is as noisy a target as it is for
the path tracer. The prefiltered environment the lighting pass reads is a separate image;
the trace keeps the source so its misses match the path tracer's.

{{cite ohao/gpu/vulkan/env_upload.cpp "imgInfo.mipLevels = 1;"}}

## Half resolution, and the guide

In realtime the trace runs at half resolution, rounded up, two rays a pixel: trace
pixel $q$ shades full-resolution pixel $2q$, reading its position, normal and
albedo from the G-buffer. Both are settings (`render.settings`' `rtGI`: resolution
`half | full`, 1–32 rays); the shift that maps $q$ to its full-resolution pixel is
pushed as a constant (1 at half, 0 at full), and the denoiser's reprojection and
upsample take the same shift.

{{cite shaders/rt/gi_trace.slang "const uint2 full = min(reference ? pixel : pixel << pc.flags.w, pc.extent.zw - 1u);"}}

:::why
The half-resolution reprojection keeps its original expression literally behind a
branch instead of folding the shift into a multiply by 0.5 or 1.0: the folded form let
the compiler contract differently and moved five deferred goldens by one LSB.
:::

Alongside the noisy radiance it writes a guide word for the denoiser, packed as RT
shadows pack theirs: the distance from the camera as a bfloat16 (a float's top 16
bits, never zero on a surface) and the octahedral normal at 8 + 8 bits. Zero means
sky.

{{cite shaders/rt/gi_trace.slang "guideOut[pixel] = dist | (oct.x << 16u) | (oct.y << 24u);"}}

The half-resolution images — the trace's radiance, the guide, the two accumulations
and an à-trous scratch image — stay in `GENERAL` for their whole life. The guide and
both accumulations are ping-pong pairs, this frame writing one half while reading
last frame's in the other.

## The denoiser

`rt.gi.denoise` is four dispatches: one temporal pass, two à-trous iterations and an
upsample.

**Temporal.** Each half-resolution pixel is reprojected by the G-buffer's velocity
into last frame, and last frame's accumulations are read bilinearly over the four
nearest samples — but a tap counts only if last frame's guide there is the same
surface: within 10 % of this pixel's distance, with normals within
$\cos^{-1} 0.9$. The weights of the taps that pass are renormalized, and less than
5 % of a tap in total is no history at all.

{{cite shaders/rt/gi_denoise.slang "if (abs(gp.w - gc.w) > 0.1 * gc.w || dot(gp.xyz, gc.xyz) < 0.9) continue;"}}

Two accumulations come through that reprojection. The short one blends the new
sample with weight $1/n$, $n$ capped at `kGIFastHistoryFrames` (four); the long one
the same with $n$ capped at `kGIHistoryFrames` (32). The two start and reproject
together, so one frame count serves both: the short accumulation's alpha keeps it
(capped at 32, the short blend capping its own use at four), and after a reset or a
disocclusion the count restarts and the first frames average evenly. That frees the long
accumulation's alpha for the visibility, which accumulates with the long blend.

{{cite shaders/rt/gi_denoise.slang "const float n = fast.a;   // min(frames of history + 1, kGIHistoryFrames)"}}

The long accumulation is what the frame shows, and on its own it would trail any
change in lighting by tens of frames. So before blending, its history is clamped to
the range the short accumulation spans over the 3×3 neighbourhood — surface pixels
only — and a moved or switched-off light drags at most a few frames behind:

{{cite shaders/rt/gi_denoise.slang "const float3 slow = lerp(clamp(slowPrev.rgb, lo, hi), cur.rgb, 1.0 / n);"}}

To read that neighbourhood, each 16×16 workgroup (`kGITile`) first computes the
short accumulation for its pixels and a one-pixel apron into shared memory.

**À-trous.** Two iterations of a 5×5 B3-spline kernel over the long accumulation,
at step 1 and then step 2, so the second reaches four pixels out and the pair
together spans 13×13 half-resolution pixels, 26 across at full resolution. A neighbour's
weight falls with its depth difference, against a tolerance of 3 % of the centre's
distance that grows with the offset, and — only when the two packed normals differ —
with the sixteenth power of their cosine.

{{cite shaders/rt/gi_denoise.slang "const float invTol = 1.0 / (0.03 * gc.w * float(step));"}}

The second iteration writes over the trace's radiance image, which nothing reads by
then.

**Upsample.** Each full-resolution pixel takes the four nearest half-resolution
samples, weighted bilinearly and by how well each matches the pixel's own depth
(5 % of its distance) and normal (cosine to the eighth). Where no sample is the same
surface — a sliver the half-resolution trace never landed on — it takes the closest
match instead of a blur across the edge. The à-trous iterations and the upsample filter
the visibility with the radiance, all four channels alike.

{{cite shaders/rt/gi_denoise.slang "uOutput[pixel] = wsum > 1e-3 ? sum / wsum : best;"}}

Every output depends only on its inputs — no atomics, no order — so a denoised frame
is the same in every run that traced the same rays.

## Reference quality

`RenderQuality::Reference` is the converged answer the realtime frame approximates.
The same trace runs at full resolution with `kGIReferenceRays` (32) rays a pixel and
no denoiser, and keeps the plain running mean of the reference frames since the last
temporal reset in an `RGBA32F` image created on first use. The first of them takes
its own value and reads nothing.

{{cite shaders/rt/gi_trace.slang "const float4 m = prev + (mean - prev) / (float(k) + 1.0);"}}

It writes the mean (radiance and visibility) straight to the output, and the renderer
adds no `rt.gi.denoise` pass:

{{cite ohao/render/deferred/deferred_renderer.cpp "if (!m_rtGI->denoises()) giToLighting();   // reference: the trace is the output"}}

$k$ is not the renderer's frame index but the technique's own count of the reference
frames in the mean, `m_refFrames`, pushed in `flags.z`. A temporal reset zeroes it,
and so do a new image (the first reference frame, or one after a resize, whose
contents the barrier discards) and any realtime frame, which breaks the run. A live
session that switches from realtime to reference therefore starts a fresh mean
instead of blending into whatever an earlier reference run left in the image —
`sp2b_t10_a_reference_mean_restarts_after_realtime_frames` checks it, and RT shadows'
means follow the same rule.

{{cite ohao/render/rt/rt_gi_technique.cpp "m_refFrames = 0;   // a realtime frame breaks the reference run"}}

## Where the bleed lands

The lighting pass raises flag bit 3 when it holds a GI view and adds the radiance,
times the diffuse albedo and AO, to the ambient term (above). The constant ambient term
does not apply with it: GI's rays are the frame's only indirect diffuse light.

GI and image-based lighting are not simply additive. A GI ray that leaves the scene
already brings back the environment, occluded by everything it passed; the lighting
pass's diffuse IBL term is that same light unoccluded, so with GI in the frame it is
dropped. The specular IBL stays, and GI decides what it sees: the prefiltered
environment weighted by the visibility, and the radiance the rays that hit brought back
for the rest (the lighting unit, "Image-based lighting"):

{{cite shaders/core/deferred_lighting.slang "const float3 incoming = prefiltered * gi.a + (rtGI ? max(gi.rgb - irradiance * gi.a, float3(0.0)) : float3(0.0));"}}

SSAO multiplies the IBL terms and the constant ambient (when a frame without GI has
it), never the GI image: a GI ray
already stops at the occluder SSAO would be guessing at, so occluding its result again
would count the same crease twice.

The handover is per frame and conditional on rays having been traced. Before the
graph executes, the renderer clears the lighting pass's GI view; one lambda hands
the output back, called from `rt.gi.denoise` in realtime and from `rt.gi` in
reference, and only when `render()` dispatched — it returns early with no TLAS, no
instance table, or, in realtime, no velocity view:

{{cite ohao/render/deferred/deferred_renderer.cpp "if (m_lightingPass && m_rtGI->rendered())"}}

The lighting pass, registered after both, reads that view at its own record time —
once to raise flag bit 3, and once in its descriptor write, which picks the real view
over the dummy fallback.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "if (m_ssgiView != VK_NULL_HANDLE) m_params.flags |= 8;"}}

A frame that traced nothing therefore renders with no indirect term rather than with
a stale or never-written GI image.

## Rays, seeds and cost

Every random number is an integer PCG hash of the render seed, the frame index since
the last temporal reset and the pixel, hashed once per pixel, then of the ray and the
dimension:

{{cite shaders/rt/gi_trace.slang "return pcg(pc.seed.x ^ pcg(pc.seed.y ^ pcg(pixel.x ^ pcg(pixel.y))));"}}

{{cite ohao/render/rt/rt_gi_technique.cpp "pc.seed = u4(input.renderSeed, input.frameIndex, m_rays, lightCount);"}}

So the noise decorrelates from frame to frame — which is what lets the temporal
accumulation converge — and two renders of the same world with the same seed trace
the same rays. The history is dropped on frame 0 after a temporal reset
(`resetTemporalState` zeroes the count and calls `resetHistory`), on a resize, and
by any frame rendered in reference quality.

{{cite ohao/render/rt/rt_gi_technique.cpp "if (input.frameIndex == 0) resetHistory();   // a temporal reset: no history"}}

Realtime traces two rays per half-resolution pixel, half a ray per output pixel;
reference traces 32 per output pixel. Measured with `render_bench` on an RTX 5070
Laptop GPU (2026-10-25), median of 100 frames: on `cornell_720p` `rt.gi` 0.064 ms and
`rt.gi.denoise` 0.143 ms, within budgets of 0.30 and 0.20; on `helmet_game_720p`
0.070 and 0.151 ms; on `helmet_game_1080p` 0.164 and 0.339 ms. The denoiser costs
about twice the trace at every size.

## Sharp edges

The ray origin is pushed a flat 0.05 world units along the shading normal, with
`TMin` 0.001 behind that, and the ray ends at 100 units:

{{cite shaders/rt/gi_trace.slang "const float3 origin = p + n * 0.05;"}}

Both are absolute: an occluder nearer than the offset is invisible to the integral,
a surface farther than 100 units counts as a miss and is replaced by the environment
behind it, and both quietly assume a metre-ish scene scale.

Sky pixels are detected by testing all four channels of the G-buffer position for
exact zero, in the trace and again in the upsample. The fourth channel is metallic,
so a non-metallic surface passing exactly through the world origin gets no GI.

{{cite shaders/rt/gi_trace.slang "if (all(position == float4(0.0)) || position.w < 0.0)   // sky, or a light's emitter mesh: no indirect light"}}

A negative metallic is a light's emitter mesh, which the G-buffer writes as pure emission:
nothing lights it, so it takes no GI either, and its guide reads as sky.

The trace queries with ray mask `kRayMaskScene`: every visible scene instance, neither
hidden ones (mask 0) nor light emitter meshes, so a bounce never reaches a light's mesh and
adds the light a second time (hybrid/visibility). The query is not forced opaque: a
candidate from an alpha-tested or blended instance is committed only where
`rt/alpha_test.slang` says it is hit, so a cutout bounces only where it has material and a
blended surface bounces nothing.

{{cite shaders/rt/gi_trace.slang "RayQuery<RAY_FLAG_NONE> q;"}}

The bounce is a directional albedo at normal incidence, not the hit's BSDF toward the
ray's origin: a glossy hit bounces the same light whichever way it is seen from, and its
specular reflection of the light is spread as if diffuse.

:::key
The Monte-Carlo layer is textbook: cosine-weighted, stratified, unbiased for the
hemisphere it samples. Every approximation lives in what a ray may know when it
lands — one albedo per hit at normal incidence, one light shadowed per hit, a
normal guessed from the ray — and every frame-to-frame artifact in what the
denoiser keeps: a history clamped to four frames' range, tested per tap against
depth and normal.
:::

## Contracts

- The output is incoming radiance; the lighting pass applies the receiver's diffuse directional albedo (`OpenPBRBase::diffuseAlbedo`). Any consumer of binding 11 must not multiply by albedo again, and one that also adds unoccluded diffuse IBL counts the environment twice.
- A hit's material is resolved by `hitMaterial` from the instance at the TLAS custom index, which is the `RenderScene` instance slot, through its material refs; the material rows it reads (0–2, 4–6) must stay what the G-buffer and the path tracer read. The instance stride is tied to the C++ struct by a `static_assert` in `rt_gi_technique.cpp`.
- Realtime needs the G-buffer velocity view: `render()` returns without tracing when it is null, and then nothing is handed to lighting. `denoise()` must follow `render()` in the same frame and does nothing unless `denoises()`.
- `setSSGITexture` must run after the per-frame reset and before the lighting pass records, and only when `rendered()` is true — from `rt.gi.denoise`'s callback in realtime, from `rt.gi`'s in reference, both registered before `lighting`.
- Each pass's descriptor sets are written at record time into the sets for the caller's `frameSlot`, one of `MAX_FRAMES_IN_FLIGHT`, and the slot's light buffer is rewritten in `render()`. Both hold only while the caller passes the frame-ring slot whose fence it has just waited on.
- `GIInput::frameIndex` and `GIInput::renderSeed` are the only sources of sample variation, and frame 0 takes no history. A caller that passes a constant non-zero frame index freezes the noise in screen space.
- Reference quality's running mean holds only the reference frames in an unbroken run: a temporal reset, a new image (a resize) and any realtime frame start it again. The shader trusts `flags.z` alone; anything else that writes `m_refMean` must zero `m_refFrames`.
