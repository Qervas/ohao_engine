---
module: deferred
id: ssao
title: SSAO
standard: v2
---

## A heightfield instead of rays

Ambient occlusion is a visibility integral: over the hemisphere above a shading
point, what fraction of directions escape to the sky? Answering it honestly costs
rays. SSAO is the answer you get without them — a single-layer heightfield of
whatever the camera can see, read out of the G-buffer — and everything on this page
is a consequence of trying to read occlusion out of that heightfield.

It is not, in this engine, the occlusion a frame leans on for direct light. RT
shadows occlude direct light (CSM where they cannot trace), and RT GI's rays already
see the occluders between a surface and its bounce light. What neither touches is the
constant ambient term and the image-based environment terms: those are lit from every
direction with nothing in the way, and that is the light SSAO darkens. It is opt-in —
the pipeline gate defaults false:

{{cite ohao/render/deferred/post_processing_pipeline.cpp "if (m_ssaoEnabled && m_ssaoPass) {"}}

and the callers that open it are `model_viewer`'s deferred path, the `ohao_game`
host, a live window session that turned it on, and a capture whose `post` list names
`ssao`. When it is off the renderer registers no `ssao` pass and clears the lighting
pass's SSAO view, so the frame has neither the cost nor the timing:

{{cite ohao/render/deferred/deferred_renderer.cpp "if (!ssaoOn && m_lightingPass) m_lightingPass->setSSAOTexture(VK_NULL_HANDLE, VK_NULL_HANDLE);"}}

## Two dispatches at half resolution

`SSAOPass` is two compute entry points of one Slang module,
`compute/ssao.slang` — `occlusion` and `blur` — loaded through the same `rtc` helper
the RT techniques use and timed together as `ssao`:

{{cite ohao/render/deferred/ssao_pass.cpp "if (!rtc::createComputePipeline(m_device, "compute_ssao.slang.occlusion.spv", "occlusion", m_occlusionSetLayout,"}}

Both write `R16G16_SFLOAT` images at half the frame's size, rounded up: the
occlusion before the blur, and the blurred output lighting reads. Each holds a pair,
the occlusion and the pixel's distance from the camera (zero for sky), and the
distance is the depth guide for everything downstream.

{{cite ohao/render/deferred/ssao_pass.hpp "static constexpr VkFormat kOutputFormat = VK_FORMAT_R16G16_SFLOAT;"}}

Half-resolution pixel $q$ shades full-resolution pixel $2q$, reading its world
position and octahedral normal straight from the G-buffer. Nothing is reconstructed
from depth, and position and normal are both world space, so the estimator works in
one frame throughout. A pixel whose position is exactly zero in all four channels is
sky and writes (1, 0):

{{cite shaders/compute/ssao.slang "const int2 p = min(int2(q * 2u), full - 1);"}}

## A horizon search in the tangent plane

The occlusion pass is a horizon search. It builds a tangent basis around the
normal and walks `config.x` directions in that plane — eight by default — each
marched `config.y` steps (four) across the screen toward the projection of a point
`radius` metres away (0.5 m), through the unjittered view-projection:

{{cite shaders/compute/ssao.slang "const float4 clip = mul(pc.viewProj, float4(pos + dir * radius, 1.0));"}}

Every sample is a full-resolution G-buffer position. Seen from the shaded point
lifted `bias` along its normal (0.025 m), its elevation is the sine of its angle
above the tangent plane; a direction keeps its highest elevation, faded by the
sample's distance within the radius:

{{cite shaders/compute/ssao.slang "best = max(best, elevation * (1.0 - l2 / (radius * radius)));"}}

With $D$ directions, $s$ the samples of direction $d$, $\theta_s$ a sample's elevation
above the tangent plane, $\ell_s$ its distance from the lifted origin, $r$ the radius
and $k$ the intensity, the pixel's ambient visibility is

$$\mathrm{AO} = \operatorname{saturate}\!\left(1 - \frac{k}{D}\sum_{d=1}^{D} \max_{s \in d}\; \sin\theta_s \left(1 - \frac{\ell_s^2}{r^2}\right)\right)$$

A direction with nothing above the tangent plane contributes zero, so a flat,
unoccluded floor comes out exactly 1 — the property the old view-space estimator
lacked. Samples outside the radius, off the screen, on the sky or on the pixel itself
are skipped, and a direction whose end point falls behind the camera is skipped
whole while still counting in $D$. The result then fades back to 1 between 50 and
300 m from the camera (`falloffStart`, `falloffEnd`), where a 0.5 m radius covers less
than a pixel:

{{cite shaders/compute/ssao.slang "ao = lerp(1.0, ao, 1.0 - smoothstep(pc.params.z, pc.params.w, dist));"}}

The lifted origin is what `bias` is for: samples of the shaded surface itself, which
sit in the tangent plane, end up slightly *below* the lifted point and so add
nothing, instead of registering as a grazing occluder through half-float position
error.

## Sixteen rotations, every texel used

Eight fixed azimuths band. Each pixel therefore rotates its directions by a fraction
of the angle between two of them, taken from a 4×4 interleaved pattern indexed by the
half-resolution pixel, texel $(q_x \bmod 4,\, q_y \bmod 4)$:

{{cite shaders/compute/ssao.slang "const uint k = (q.x & 3u) | ((q.y & 3u) << 2u);"}}

The sixteen rotations are a permutation of the sixteen strata $(k + 0.5)/16$, so any
4×4 window holds sixteen evenly spaced rotations. The permutation is a Fisher–Yates
shuffle driven by a counter hash of the render seed (`VulkanRenderer::setRenderSeed`,
default 0) — integer arithmetic and exact floats, the same on every compiler and
machine:

{{cite ohao/render/deferred/ssao_pass.cpp "for (uint32_t i = 15; i > 0; --i) std::swap(order[i], order[next() % (i + 1)]);"}}

They travel in the push constants (`SSAOPush`, defined once in
`shaders/shared/layout.json`), not in a noise texture, so reseeding takes effect on
the next frame and uploads nothing. Each rotation is its stratum index, four bits, so
the sixteen fit in two uints and the block in the 128 bytes every device guarantees:

{{cite ohao/render/deferred/ssao_pass.cpp "for (uint32_t i = 0; i < 16; ++i) packed[i / 8] |= static_cast<uint32_t>(n[i] * 16.0f) << ((i % 8) * 4);"}}

`deferred_determinism_gpu_test` pins the bytes of `SSAOPass::noise(0)` and checks that
the sixteen values are the sixteen strata.

## The blur, and the upsample in lighting

The `blur` entry averages a 4×4 window, offsets −1 to 2: exactly one period of the
noise, so each window holds each of the sixteen rotations once and the interleaving
averages out. A tap's weight falls linearly with its distance difference from the
centre, to zero at 5 % of the centre's distance; sky taps are skipped, and the centre
always counts, so the sum is never empty:

{{cite shaders/compute/ssao.slang "const float w = saturate(1.0 - abs(v.y - c.y) * invTol);"}}

The lighting pass reads the blurred image at binding 10 and raises flag bit 1 (the
value 2) when it holds one. Each full-resolution pixel takes the four nearest
half-resolution samples, weighted bilinearly and by the same 5 % distance match; where
none is the same surface it takes the closest instead of blurring across the edge:

{{cite shaders/core/deferred_lighting.frag "return wsum > 1e-3 ? sum / wsum : best;"}}

## What it darkens

The upsampled value multiplies only the light that arrives from everywhere: the
constant ambient term and, with an environment map, the IBL terms. Direct light is
occluded by RT shadows or CSM, and RT GI's rays already see what is in the way, so
neither is touched:

{{cite shaders/core/deferred_lighting.frag "vec3 ambient = vec3(pc.ambientIntensity) * albedo * ao * ssao;"}}

{{cite shaders/core/deferred_lighting.frag "const float occlusion = ao * ssao;"}}

`ao` there is the G-buffer's baked occlusion (albedo alpha: 1, or the red channel of the
material's glTF occlusion texture); `ssao` multiplies on top of it. The diffuse IBL takes
the product as it is, the specular IBL through `specularOcclusion`, which occludes a
reflection's narrower lobe less. Test 5 in
`engine_tests` checks both halves: with only ambient light, a crease frame has 3191
pixels darker with SSAO on and none brighter; lit by a point light with zero ambient,
the frame is unchanged (max_abs 0). A second test renders an SSAO frame from two fresh
renderers and finds them bit-identical.

## Cost

Measured with `render_bench` on an RTX 5070 Laptop GPU (2026-10-25, a run flagged
noisy by outside load), `ssao` on `helmet_game_1080p` took 0.165 ms; the full-resolution
pass it replaced took 0.817 ms. Both dispatches are 8×8 workgroups over the
half-resolution image: at most 8 × 4 = 32 position fetches a pixel for the occlusion,
16 for the blur.

## Sharp edges

Sky is detected by exact zero in all four channels of the G-buffer position, the
fourth being metallic, so a non-metallic surface passing exactly through the world
origin is sky to SSAO as it is to every other reader.

The radius, bias and falloff are absolute metres: a 0.5 m radius is a crease detail
in a room and invisible in a canyon. `setRadius`, `setBias` and `setIntensity` exist,
and `PostProcessingPipeline` forwards radius and intensity, but nothing in the tree
calls them, so every frame runs the defaults.

It is still a heightfield. A sample within the radius counts by its elevation alone,
with no thickness test, so a thin object in front of a wall occludes the wall as if
it were solid to the depth buffer's far side; and anything off screen or behind the
first surface does not exist.

The pass has two entry points in one module and is not hot-reloadable:
`reloadShader` logs and returns false.

:::key
SSAO is now a consumer-visible term, and a narrow one: a half-resolution horizon search
in world space, sixteen seeded rotations from the push constants, a one-period blur,
and a depth-aware upsample into the ambient and IBL terms only. Nothing it computes can
darken direct light or RT GI, and a frame with neither ambient nor environment light
is bit-identical with it on or off.
:::

:::why
The images are kept outside the render graph's barrier ledger: the pass transitions
both itself, discarding last frame's contents on the way to `GENERAL` and ending in
`SHADER_READ_ONLY_OPTIMAL` for the compute and fragment readers.

{{cite ohao/render/deferred/ssao_pass.cpp "rtc::transition(cmd, m_output, kReadOnly, kCompute, VK_ACCESS_SHADER_WRITE_BIT, kReaders, VK_ACCESS_SHADER_READ_BIT, false);"}}

The graph declares the pass's G-buffer normal read and nothing else — not the position
read, not either image — so the pass is drop-in for any caller with a command buffer.
The price is that the ledger cannot check it. Discarding each frame also means no
frame amortises its taps against the last; the blur, not history, removes the noise,
so the result is the same in every frame of a still camera.
:::

## Contracts

- `execute()` does nothing until `setInputs()` has the G-buffer's position and normal views; `DeferredRenderer::render` hands them, with the unjittered view-projection and the camera position, through `PostProcessingPipeline::setSSAOInputs` every frame before the graph runs. Both inputs must be in `SHADER_READ_ONLY_OPTIMAL` when the pass executes, which the G-buffer render pass leaves them in.
- The output is half resolution, rounded up, `(ao, distance from the camera)`, zero distance for sky. Any reader must upsample it against a full-resolution distance and treat distance zero as no sample, as `ssaoAt` does.
- Lighting applies SSAO only to the constant ambient and IBL terms. A new ambient-like term belongs inside that product; direct light and RT GI must stay outside it, or occlusion is counted twice.
- The noise is a function of the render seed alone. Anything that changes `SSAOPass::noise` changes every SSAO frame and must update the pin in `deferred_determinism_gpu_test`.
- Descriptor sets are written at record time, one pair per frame in flight, so `frameIndex` must be the frame-ring slot whose fence was just waited on.
- `onResize` waits for the device idle and recreates both images; the descriptor sets are rewritten every frame, so nothing else needs refreshing.
