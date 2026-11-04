# Sub-project 24 — Caustics: light traced from the sun, then manifold sampling

Status: accepted by Frank 2026-11-03 ("go"; the OPEN rulings start at their recommendations, C1 and C4 to be ruled on
step 1's measurements); written the same day (documents only; no code), on Frank's ruling that day. He rejected the lossy fixes for the
showcase's bright dots ("oidn will make it like oil paint, not sharp as a real photo. we need real solution, not
approximation") and chose this route over VCM ("i like your new plan"). Judgment calls are written under "Rulings for
Frank" as **PROVISIONAL** (recommended; the design follows them) or **OPEN**. The spec is binding once Frank accepts it.

Roadmap: a new row 24 of `docs/ROADMAP.md`, built before sub-project 13's step 2 (ragdolls) as 23 was. Done when the
offline path tracer renders light reflected and refracted from the sun by smooth surfaces onto matte ones as a smooth,
exact result instead of rare bright dots, with the estimate unchanged: the closed forms and the camera-only tracer
agree with it.

Builds on:
- **Row 17, OpenPBR** (`2026-10-29-openpbr-materials-design.md`): `OpenPBRSurface`'s `evalLobes()`, `sample()` and
  `pdf()`, glass (`sampleTransmissive`, lobe 4) and the coat.
- **Row 18, the frontier renderer** (`2026-10-30-frontier-renderer-design.md`): the one path tracer
  (`shaders/rt/path_tracer.slang`), its next-event estimation, a directional light as a delta (`LightSample.pdf` 0),
  integer sums for order-free results (the radiance cache), raygen's one-trace rule.
- **Row 23, path guiding** (`2026-11-02-path-guiding-design.md`): kept, off by default. Guiding learns light that
  arrives off broad surfaces; this sub-project handles light that only a smooth surface can bring. They cover
  different paths and work together.

## Why (what the renderer does today)

The path tracer traces from the camera only. A shaded matte point receives the sun's reflection off a smooth surface
(the helmet's metal, glass, a coat) only when its random bounce happens to hit the spot on that surface that mirrors
the sun toward it. The sun is a delta, so that spot is as small as the surface's roughness makes it, and for a
perfectly smooth surface it has no size at all. Those rare hits are hundreds of times the pixel's mean: the dots. More
samples do not remove them: from 2048 to 8192 spp the count of dots stayed at about 12,000 (sub-project 23's
measurement).

Measured 2026-11-03 on the showcase (`yb.scene.json`: Khronos Sponza under the physical sky, the DamagedHelmet on a
coated plinth, clear and amber glass balls, a gold ball, a car-paint ball, a thin-film bubble), 480x270, 1024 spp,
`rt_offline`, no denoiser. Each object was removed in turn with the sample sequence unchanged, so a dot that
disappears was caused by that object:

| the dot (shaded floor) | without the helmet | without the glass balls | without the gold and car-paint balls |
|---|---|---|---|
| 4 dots right of the glass ball, 0.06-0.10 (floor 0.004) | gone | unchanged | unchanged |
| (158, 261), 0.071 | gone | unchanged | unchanged |
| (295, 261), 3.40, about 1000x the floor | unchanged | gone | unchanged |
| 3 dots at (213-222, 232-233) | unchanged | unchanged | gone |

Pixels beside the clear ball that look like dots are not noise: they hold the same value in guided, unguided and
8192 spp renders (0.159 against 0.160). They are the lit floor seen through the ball. Path guiding (row 23) did not
change the dot count (7 against 11 at 1024 spp, within chance). Its lobes are too broad to aim at a glint, and the
only paths that find the glint, which it would learn from, are the dots themselves.

Light traced from the sun finds these paths without difficulty: a sun ray that meets the helmet reflects to exactly one
spot on the floor. That is the thesis.

## Alternatives weighed (and why this route)

Before deciding, the plan was argued against (Frank's standing rule, 2026-11-03):
- **OIDN, a clamp.** Rejected by Frank: the first is a learned guess (lossy), the second drops real light (biased).
- **VCM** (Georgiev, Křivánek, Davidovič, Slusallek 2012; RenderMan's PxrVCM) covers every path, but its photon
  merging blurs light seen through glass within a radius. That blur shrinks to nothing only in the limit (consistent,
  not exact), and it falls on the very case where the other techniques fail. It also needs every material, light and
  feature built for both directions before it pays.
- **Full bidirectional path tracing** (Veach and Guibas 1995) is exact and covers paths with several matte bounces
  after the glint. It still cannot connect light seen through smooth glass from a delta sun, and it is the largest
  build. Kept as a conditional step 3, built only if measurement shows those paths matter.
- **ReSTIR PT** (Lin et al. 2022) reuses rare bright paths across pixels: a dot becomes a blotch.
- **What production renderers do**: camera-only tracing, path guiding, and manifold sampling for glass (Weta's Manuka:
  manifold next-event estimation, Hanika, Droske, Fascione 2015; Blender Cycles' shadow caustics since 3.2). The route
  chosen here follows that, with light tracing added for reflections off smooth surfaces onto matte ones from the sun,
  which manifold sampling handles poorly on a detailed mesh like the helmet.
- **Specular manifold sampling** (Zeltner, Georgiev, Jakob, "Specular Manifold Sampling for Rendering High-Frequency
  Caustics and Glints", SIGGRAPH 2020) is the exact, unbiased form of manifold sampling. It covers the case light
  tracing cannot: light through or off smooth glass reaching a point the camera sees only through glass. It is step 2.

## Thesis: give each light path to the technique that can find it, and count it once

Split the light reaching the camera by path. The paths the camera-side tracer cannot find become the job of a pass
that traces from the sun, and the camera-side tracer stops counting them. Every path is then counted by exactly one
technique, each technique is unbiased for its share, and the sum is the exact image. Nothing is clamped, blurred or
learned.

## Goal

- The paths **sun → one or more smooth bounces → a matte surface the camera sees directly** are estimated by light
  traced from the sun (step 1). On the showcase this removes every dot caused by the helmet, the gold ball and the glass
  balls' light onto the floor.
- The paths **sun → smooth glass or mirror → matte → smooth glass or mirror → camera** (light seen through glass) are
  estimated by specular manifold sampling (step 2).
- Paths with several matte bounces after the glint stay with the camera-side tracer (and guiding). Bidirectional
  tracing is built only if, after steps 1-2, a measured share of the showcase's remaining dots comes from them (step 3).

## Scope

In: the offline profile (`rt_offline`); every directional light as a light source of step 1 (each with its own
disks, and the camera side drops exactly those lights' owned terms); the OpenPBR
materials as they are; the agent surface; tests; the showcase.

Out (stated so nobody builds it by accident): the realtime profile (its 2 bounces and radiance cache stop these paths
anyway, and its denoisers smooth them); point, spot, area and emissive lights as sources in step 1 (their caustics stay
with the camera-side tracer; the same machinery extends to them later if measured to matter); volumes; spectral
dispersion through the light-traced pass (a dispersive glass's light is traced at the path's channel as the camera
side does it; see "The light pass").

## The design

### The split: smooth and rough parts of each surface

At every vertex the BSDF is split into a **smooth part** (lobes whose roughness is below `kCausticRough`: specular,
coat, metal, glass transmission and reflection, thin-film) and a **rough part** (everything else that leaves the surface at the hit: diffuse, fuzz, and specular lobes at or above
the threshold), `f = f_smooth + f_rough`. **Subsurface's walk is in neither part:** the offline tracer walks it
(`subsurfaceLocal` false), so light entering a walk stays with the camera side. A bounce at D that enters a walk sets
`chainThroughput` to 0. A walk's exit vertex is Lambertian, a rough-only vertex, so it ends a chain. The light pass never
enters a walk; it connects at a subsurface material through that material's rough part only, which `evalLobes` already
gives without the walked share. The split is a property
of the vertex's material at the hit (its textures included), the same on both sides. The threshold changes only which
technique carries which path, never the image's mean. It is chosen by measurement (ruling C2).

A path's contribution is a product of the `f` at its vertices. Expanding each `f` into its two parts makes the
contribution a sum of terms, one per assignment of smooth or rough to each vertex. The light pass owns exactly the terms
of the form

    sun → S₁ … S_k (smooth part at each, k ≥ 1) → D (rough part) → camera, D the camera ray's first hit

and the camera-side tracer counts every other term.

- **The camera side drops those terms.** Alongside `throughput` raygen keeps `chainThroughput`. It gets **every factor
  `throughput` gets**, except two substitutions:
  - at the first hit D, the bounce's `f / pdf` becomes `f_rough(D) / pdf`;
  - at each later vertex, `f / pdf` becomes `f_smooth / pdf`.

  `pdf` is the density the bounce was actually drawn with (guiding's mixture density included). The factors it shares
  with `throughput` are Russian roulette's division by q, absorption inside a solid, the dispersion channel's ×3, and a
  pane's transmission. `chainThroughput` becomes 0 once any vertex's smooth part is 0. D is exactly the camera ray's
  depth-0 surface hit after its culled-face skips: a camera ray that meets an emitter mesh or the sky ends the path, and
  there is no D. At a vertex
  past the first, a directional light's sample contributes `(throughput × f − chainThroughput × f_smooth) × Le`, with
  `Le` as today (the light's choice, a pane's transmission T on the shadow ray): the sun's
  light arriving through an all-smooth chain from D, and only that, is left to the light pass. When no directional light
  is in the scene, or the pass is off, `chainThroughput` is never read and the tracer is bit for bit what it was.
- **The light pass counts only those terms.** It follows only smooth parts from the sun and connects to the camera only
  through rough parts.
- **Panes** (ruling O14, `paneMaterial`): a thin pane's straight transmission is null in both techniques (the camera
  side's shadow rays pass it, multiplying its transmission; the light pass does the same and makes no vertex there). Its
  reflection is a smooth part like any other.
- **Raygen's spill.** `chainThroughput` is one more float3 held across the trace. `OHAO_PIPELINE_STATS=1` reports
  raygen's stack before and after; growth that shows in `pt.trace` with the pass off is a defect.

### The light pass (step 1)

A new raygen entry point in the tracer's pipeline, `causticTrace`, dispatched before `pt.trace` in the pass
`pt.caustics`, only in `rt_offline` with the pass on and a directional light in the scene.

1. **Where sun rays start.** Only rays whose first hit can have a smooth part can carry an owned term, so rays are aimed
   at the **casters**: instances whose material can have a smooth part anywhere (factor or texture). A material with a
   roughness texture is a caster unless the texture's minimum over its texels is at or above the threshold (computed
   once at upload, from the texture's data). Each caster's world bounding sphere projects along the sun to a disk on a
   plane perpendicular to the sun. A ray's origin is drawn on the disks: a disk with probability proportional to its
   area, then a point uniformly on it. The origin's density is the **sum over every disk that covers it** (disks
   overlap), so the estimate stays exact. Rays start beyond the scene's bounds.
2. **The weight.** A ray starts with weight `E / p(x)` (E the light's irradiance, `colour × intensity`; p(x) the
   density per unit area of the plane, the choice among directional lights included, by `E × total disk area`). At a
   smooth vertex the light arrives along ωl (toward the light) and the pass draws the direction ωe it leaves along
   (toward the camera's side). OpenPBR is not reciprocal (layering scales by albedo at wo; the BTDF is in radiance
   form with its η² factor), so the pass always evaluates the BSDF **in the camera side's roles**: `f_smooth(wo = ωe,
   wi = ωl)`. The proposal is the smooth part's own sampler called with the roles swapped (given ωl, it draws ωe), with
   its density from `pdf()` under the same swap. Any proposal is unbiased when the weight is
   `f_smooth(wo = ωe, wi = ωl) × |cos(ωe, Ns)| / p(ωe)` times the shading-normal correction below. The proposal's shape
   must follow the lobe (geometric reflection and refraction are reversible, so for smooth lobes it does). A smooth part
   of 0 ends the ray. Russian roulette by weight from the third vertex, with its own draws.
   **Shading normals.** Light carried away from the light is not symmetric under interpolated or bump-mapped normals.
   Every light-pass vertex multiplies Veach's adjoint correction
   `|ωl·Ns| |ωe·Ng| / (|ωl·Ng| |ωe·Ns|)` (Veach 1996, "Non-symmetric scattering in light transport algorithms"; PBRT 4
   `CorrectShadingNormal`). The connection at D applies it too.
3. **Connecting.** At every vertex from the second on (after at least one smooth bounce), if the rough part there is
   not 0, the pass connects the vertex to the camera:
   - It casts a visibility ray with the camera ray's mask and its rule for one-sided back faces (`kHitCulled`, at most
     `kCullSkips` skipped), and checks that the camera ray through that pixel would meet this vertex first. Anything
     the camera ray would meet first blocks it, a pane or an emitter mesh included: a matte point seen through a pane
     belongs to step 2, not here.
   - It projects the vertex to its pixel and adds
     `weight × f_rough(wo = toward the camera, wi = ωl) × |cos(toward the camera, Ns)| × We / d² × (the correction)`.
     d is the distance to the camera; We is the pinhole camera's importance toward D for the image's box filter, PBRT 4's
     `PerspectiveCamera::We` (it holds the cos⁴ of the off-axis angle and the image plane's area), normalised so that a
     pixel's value is the mean radiance over its footprint, the camera tracer's measurement. The footprint is the one the
     camera side jitters over: the pixel offset by `pc.jitter.xy`.
     A Blend surface the camera ray would pass stochastically multiplies the connection by its expected transmittance
     `1 − α` (the camera side's stochastic draw has the same expectation). A MASK cutout passes exactly where the camera
     ray passes.
   - The connection is where the term's last vertex D meets the camera. The ray then continues through its smooth part,
     because a longer chain is another owned term.
   - Vertices behind the camera or outside the image add nothing.
4. **Splatting is integer.** Each frame's light image is summed by `InterlockedAdd` in fixed point, as the radiance
   cache sums. Order-free sums keep a capture the same bits in one process. Overflow is impossible by construction: the
   scale and word width are chosen against the largest single splat, and a splat that would not fit is split across
   words or clamped to the representable range. A clamp is measured, counted in `render.stats`, and must be 0 on every
   test scene.
5. **Adding it.** `pt.trace` reads the pixel's light image for this frame, scaled to one frame's estimate, and adds it
   to the sample before the running mean. The frame's buffer is cleared after it is read.
6. **How many.** `kCausticRaysPerPixel` light paths per frame per image pixel (start: 1). The ratio sets how the frame's
   time divides between the two techniques; it is chosen by equal-time measurement on the showcase (ruling C3).
7. **Glass.** Smooth refraction through a solid follows the same rules as the camera side: absorption inside by
   `transmission_color` at `transmission_depth`, total internal reflection, crossings counted against `kCrossings`, not
   bounces. Dispersion draws a channel at the first dispersive glass and carries one channel, as raygen does.
8. **AOVs.** In offline NRD (`tracerAccumulatesAOVs`), the light image goes into the diffuse or specular radiance by
   which lobe of D's rough part it came through. OIDN takes it as part of the colour.

### Specular manifold sampling (step 2)

Its own design section is written at the start of step 2 (the spec is extended in the same commit). The outline it must
keep:
- The paths it owns are those the light pass cannot connect: the camera's path reaches a matte point D only through
  smooth vertices (seen through glass or in a mirror), and D receives the sun through one or more smooth surfaces
  (refraction through a ball, reflection off a mirror).
- From D it solves for a chain of smooth-surface points connecting D to the sun direction (Newton iterations on the
  surfaces' positions and normals, interpolated normals included), and estimates the chain's probability without bias
  by Zeltner et al.'s reciprocal-probability estimator (Bernoulli trials).
- It is exact for any number of solutions; MNEE's simpler one-solution form (Hanika et al. 2015) is biased where there
  are several, so it is not used.
- It is split from the other two techniques by the same smooth and rough parts, so every path stays counted once.
- Chains are limited to two smooth interfaces in step 2 (a glass ball: in and out). Longer chains stay with the
  camera-side tracer.

### Bidirectional tracing (step 3, conditional)

Built only if, after step 2, measurement attributes a share of the showcase's remaining dots to paths with matte
bounces after the glint (helmet → floor → wall), above a bar Frank sets (ruling C6). If built, it reuses the light
pass's paths and splatting.

## The agent-facing surface

- `render.settings {caustics: off | on}`, `render.capture {caustics}`, `ohao run --caustics`. Default: ruling C1.
- `render.stats` reports `pt.caustics`' timing and, when the pass ran, `caustics: {rays, casters, splats, clamped}` for
  the last frame.
- The schema hash moves with the new setting (every CLI and serve expectation that records it, checked to differ only
  there).

## Determinism

- Pass off: bit-identical to today: no draw, buffer or branch of the existing path changes. Every golden unchanged.
- Pass on: its own Sobol (or PCG) stream seeded by the frame and the light-path index, integer splats, a fixed
  dispatch, so two captures in one process are the same bits. Across processes the BVH floor applies as today (a ray on
  a shared edge may take either triangle); measured and stated.

## Performance budgets

Proposals, `pending` until Frank rules:
- The showcase at 1080p: `pt.caustics` at most 0.5x `pt.trace` per frame at `kCausticRaysPerPixel` 1;
- An offline scene without directional light, or without casters, is unchanged: `pt.caustics` is not dispatched.

## Test plan

Numbers continue as `c1`... (`tests/renderer/check_caustics.py` unless noted):
- **c1, a mirror's closed form.** A matte floor, a wall blocking the direct sun, and a perfect mirror (roughness 0,
  metal of known F0) that reflects the sun onto the shaded floor. The floor's radiance there is
  `albedo/π × E × F(θ) × cos θ'`, a closed form. With the pass it is within 2%. Without it, at equal spp, the
  camera-side tracer's estimate there is mostly 0 with rare dots (OpenPBR floors roughness at 0.01, so the glint is
  tiny, not absent); the test records that relative error too, so it shows the pass is what lights the floor.
- **c2, the split counts each path once.** A glossy reflector (roughness 0.15, below the threshold) where the camera
  side alone converges: the scene at 2^16 spp camera-only against the scene with the pass at 2^12 spp. Means within 1%
  overall and within the noise per 32x32 block, the bar computed from each image's own variance.
- **c3, glass.** A wide horizontal slab of solid clear glass (IOR 1.5, roughness 0, no absorption) over a matte floor,
  the sun at 30° from the zenith, the floor seen only directly (not through the slab). Every internally reflected beam
  still lands on the floor, so the floor under the slab's middle is lit uniformly. With F the exact dielectric Fresnel at
  the sun's angle (the same F at the inner face by symmetry; the renderer is unpolarised, so F is the average of s and p),
  the irradiance is `E cos θ (1 − F) / (1 + F)`: every order of internal reflection summed. Within 2%. The glass ball in
  the showcase is the visual test (c5).
- **c4, the camera through-path rule.** A floor point visible to the camera only through glass gets nothing from the
  pass (it is step 2's), and its value with the pass on equals its value with the pass off at equal spp, within noise.
- **c5, the showcase.** 960x540: pixels over 8x their 5x5 median (plus 0.01) on the shaded floor and plinth, with the
  pass at N spp against without it at N and at 4N, at equal time. The bar is ruling C4. Every dot the ablation above
  attributed to the helmet, the gold ball and the glass balls is gone.
- **c6, determinism and no change when off.** Two captures with the pass in one process equal bit for bit; every golden
  unchanged with it off.
- **c7, the rest of the referee with the pass on.** `check_material.py` (the furnace, the closed box) and
  `check_analytic.py` within their bars with `--caustics`. Validation clean (`check_validation.py` gains a capture with
  the pass).
- Step 2's tests (`c8`...) are written with its design section.

## Acceptance (from Frank)

The showcase at 1080p with the pass, compared full size with the render without it at equal time: the shaded floor
around the helmet and the glass balls is a smooth glow, sharp, with no dots.

## Done criteria

Step 1: c1-c7 pass, every gate green, the showcase shown to Frank, a change record. Step 2: its tests pass, the
through-glass dots gone, a change record. Step 3 only on ruling C6.

## Build order

1. **Step 1, light traced from the sun:**
   - the smooth/rough split in the module (`evalLobes` gives the two parts; a smooth-only sample and density);
   - `chainThroughput` in raygen;
   - casters and their disks, with the texture-minimum test at upload;
   - `causticTrace`, the integer splat and its read in `pt.trace`;
   - the setting, the capture flag and the CLI;
   - c1-c7, the measurements (C2, C3) and the showcase.
2. **Step 2, specular manifold sampling:** its design section, then the build and its tests.
3. **Step 3, bidirectional tracing:** only on ruling C6.

## Risks

- **The split's two sides must agree exactly.** A term dropped by the camera side and not added by the pass (or both) is
  bias. c1-c3 are built to catch it: each has a closed form or a camera-only reference.
- **Casters bounding the whole scene.** If Sponza's own textures fall below the threshold, Sponza is a caster and the
  disks cover the scene. That is still exact, but most sun rays then hit diffuse stone and add nothing. Measured; the
  threshold (C2) and the texture-minimum test are the remedies.
- **Raygen's stack.** A second raygen in the same pipeline may raise the pipeline's stack size for both. Measured with
  `OHAO_PIPELINE_STATS=1`; if it does, the pass gets its own pipeline.
- **Splat contention.** Many light paths landing on the same few pixels (a focused glass caustic) contend on atomics.
  Measured in `pt.caustics`' time.
- **The BVH floor across processes** (Determinism).

## Review focus

That the split is exact on both sides: `chainThroughput` at the first hit and at each smooth vertex, the dropped term
at a sun sample, the pass's ownership of exactly `sun → S+ → D → camera`. Then the camera importance `W` and the box
filter, the disks' summed density, panes as null in both, and the integer splat's range.

## Step 1 as built (2026-11-04)

Spec corrections the build made (each in `docs/changes/2026-11-04-caustics-step1.md`):
- **The camera side cannot render sun through glass at all.** Light sampling looks above a surface only, so a shadow ray
  through solid glass is blocked, and no bounce ray hits a delta sun. Light through glass onto a matte surface was
  missing, not only noisy: the pass removes a bias there. `c2` therefore holds the split on reflectors, where the camera
  side converges alone, and glass is held by closed forms and a ray simulation (`c3_slab`, `c3_sphere`).
- **The split lives in its own program.** `chainThroughput` behind a flag in raygen cost the default tracer 8.5% of
  `pt.trace` with caustics off; raygen is now a generic over `CAUSTICS` and `raygenCaustics`, with `causticTrace`, is a
  second pipeline made the first time a frame needs it.
- **The pass applies raygen's own bounce limit** (a chain's k-th vertex counts only while k minus the crossings before it
  is at most `bounces`), or it would count paths the camera side never reaches.
- **Casters are placed through their instance's transform.** `RenderScene::Draw`'s bounds are the mesh's own; taken raw, a
  moved caster's rays were aimed at its unmoved mesh (half a glass ball's caustic missing).
- **A pane** is a null event in the pass with its refraction share's probability (weight T over the share), its reflection
  a smooth vertex, as shadow rays take panes.
- **`render.stats` reports `caustics {casters, clamped}`**, not rays and splats (32-bit counts of millions a frame wrap).
- The splat rounds stochastically (the mean exact however small a splat).

Measured on the showcase (`assets/scenes/caustics_showcase.scene.json`, 480 x 270, against 8192 spp with the pass):
- **C3, rays a pixel**, the pass at 512 spp against none at 704 (about the same time): 1, 2, 4, 8 and 16 rays gave
  whole-image relMSE 0.00133, 0.00115, 0.00104, 0.00098, 0.00093 (without: 0.188) and typical-pixel relMSE (worst 0.1%
  out) 0.000525, 0.000458, 0.000398, 0.000360, 0.000336 (without: 0.000361), at 12.8 to 15.8 s. **8** (14.9 s): as
  good on typical pixels as no pass, 190x better overall; 16 no better per second.
- **C2, the cut**: 0.1 left the helmet's glints with the camera side (43x their neighbours; relMSE 0.00254); 0.4 equal to
  0.25 within noise (0.00094 against 0.00098). **0.25 kept.**
- The pass at 8 rays costs about 1.4x a frame on the showcase (29 ms against 20 at 480 x 270).

## Rulings for Frank

- **C1 (OPEN) — the default.** Off until c1-c7 pass, like guiding. Recommendation: on for `rt_offline` after step 1's
  tests pass, since the image converges to the same answer with fewer dots.
- **C2 (measured) — the smooth threshold `kCausticRough`.** Any value is exact; it sets which technique carries which
  paths. Measured 0.1, 0.25, 0.4 on the showcase ("Step 1 as built"): **0.25**.
- **C3 (measured) — light paths per pixel.** Measured 0.5 to 16 at equal time ("Step 1 as built"): **8**.
- **C4 (OPEN) — the bar for "no dots"** (c5). Recommendation: with the pass at N spp, no more dots on the shaded floor
  and plinth than without it at 4N, at equal or less time, and zero dots from the objects the ablation names.
- **C5 (PROVISIONAL) — sun only in step 1.** Lamps and emissive triangles keep their caustics on the camera side; the
  pass extends to them later if measured to matter.
- **C6 (OPEN) — when to build step 3.** Recommendation: only if, after step 2, more than a quarter of the showcase's
  remaining dots come from paths with a matte bounce after the glint.
