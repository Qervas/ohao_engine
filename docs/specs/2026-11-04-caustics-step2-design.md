# Sub-project 24, step 2 — Caustics: the sun through smooth surfaces onto matte ones past the first hit

Status: **accepted 2026-11-04** (Frank: "spec pass"); written the same day on his "let's design it" and "good let's
do it". Judgment calls are under "Rulings for Frank" as **PROVISIONAL** (recommended; the design follows them) or
**OPEN** (measured during the build). Step 2a's plan: `docs/plans/2026-11-04-caustics-step2a.md`. It replaces the outline of step 2 in
`docs/specs/2026-11-03-caustics-design.md` ("Specular manifold sampling (step 2)"), which chose Zeltner et al.'s
reciprocal-probability estimator before the measurement below and before the 2024-2025 work it cites.

Builds on step 1 (`docs/changes/2026-11-04-caustics-step1.md`): the smooth and rough parts (`evalParts`,
`kCausticRough` 0.25), `raygenCaustics`' `chainThroughput`, the casters' table, the order-free integer splat.

## Why: what the showcase's last dots are

After step 1 the showcase (`caustics_showcase.scene.json`, 480 x 270, 512 spp, equal time against an 8192 spp reference)
has 7 dots (pixels over 4x the reference + 0.01), against 18 without the pass. To find what they are, `raygenCaustics`
gained a diagnostic, `OHAO_PT_CLASS=<mask>`: it keeps only the light of chosen path classes, with the sample sequence
unchanged, so the class images sum to the whole image (measured: within 1.3e-6, the whole's largest pixel 2.73). A
vertex is smooth when its specular roughness is under `kCausticRough`; "after a rough bounce": some earlier vertex
scattered by a rough lobe.

| dot | x the reference | the class carrying it |
|---|---|---|
| (259, 239), at the glass ball | 10.1 | the sun at a smooth vertex after a rough bounce (0.061 of 0.063) |
| (217, 42), the arch | 10.0 | the sun at a smooth vertex after a rough bounce (0.249 of 0.257) |
| (439, 113), the pillar | 6.6 | the sun at a smooth vertex after a rough bounce (0.043 of 0.047) |
| (265, 218), at the glass ball | 6.0 | the sun at a smooth vertex after a rough bounce (0.056 of 0.063) |
| (195, 10), the arch | 6.0 | the sun at a smooth vertex after a rough bounce (0.037 of 0.042) |
| (317, 223), the floor | 5.0 | the sun at a rough vertex (0.056 of 0.068) |
| (473, 108), the plants | 4.9 | the sun at a smooth vertex, no rough bounce before (0.42 of 0.49) |

**Five of seven are one kind of path:** sun → a smooth surface (the glass ball's outside, the helmet's metal, the gold
ball) → a matte surface that is not the camera's first hit → ... → the camera. The camera side finds them only when a
rough bounce happens to aim at the spot of the smooth surface that mirrors the sun toward it. Step 1's light pass cannot
take them: it connects only to the camera's first hit. The bidirectional tracing step 1's spec held back for these
(its step 3) would connect a light path's smooth vertex to the camera path's matte one, which is the same rare event
seen from the other side (a peaked BSDF evaluated at a point drawn without regard to it): no cure.

**The other paths step 2 owns are missing, not noisy, so no dot shows them:** sun → glass (in) → glass (out) → floor,
seen past the first hit (through glass, or after a bounce). The camera side cannot find them at all (its light sampling
looks above a surface only; no ray meets a delta sun), and the light pass connects only to the first hit. They are a
bias today, as the glow under the glass ball was before step 1. (`c4_through_glass`'s R is lit by a mirror, a reflection:
seen through glass, step 2a owns it, and c4 holds it to its closed form.)

## Goal

Every path whose light leaves the sun, meets one or two smooth surfaces, and lands on a matte surface the camera's path
reaches past its first hit is found by solving for it, exactly: no denoiser, no clamp, no light left out, every path
counted once. Done when the closed forms below hold, the glass-seen caustic is found, and the showcase's five
glint-after-rough dots are gone at equal time.

## Alternatives weighed

| route | exact? | why not (or why) |
|---|---|---|
| Specular manifold sampling, unbiased (Zeltner, Georgiev, Jakob 2020) | in expectation | Newton from a random seed; the reciprocal of the success probability by Bernoulli trials has a heavy tail where solutions are hard to find: the fireflies this sub-project removes, back |
| MNEE (Hanika, Droske, Fascione 2015) | no | one solution from one seed; biased wherever there are several |
| Photon mapping, VCM | no (consistent) | a kernel's blur; Frank ruled it out ("not approximation") |
| Path regularization (Kaplanyan, Dachsbacher 2013) | no | widens the smooth lobes |
| PSMS ReSTIR (Hong et al., SIGGRAPH Asia 2025) | as SMS | SMS made fast for interactive frames; the same estimator underneath |
| **Specular Polynomials (Fan et al., SIGGRAPH 2024)** | yes (deterministic) | every solution on a chosen tuple of triangles, by resultants: exact at one bounce (Laplace expansion), bisection at two; no probability to estimate. Needs tuples chosen |
| **Bernstein Bounds for Caustics (Fan, Wang, SIGGRAPH 2025)** | yes | chooses tuples by a bound on their irradiance; the authors state the choice is unbiased. Works with either solver |

**The route:** choose triangle tuples by a conservative bound (every tuple that can hold a solution has a nonzero
probability), and within a tuple find **every** solution. Then the estimator is a plain sum over what was found divided
by known probabilities: exact, and with no reciprocal-probability tail. The solver within a tuple is measured (ruling
M1).

## The terms step 2 owns

Step 1 owns `sun → S₁ … S_k (smooth parts) → D (rough part) → camera`, D the camera ray's first hit. Step 2 owns:

    sun → S₁ … S_k (smooth parts, 1 ≤ k ≤ K) → X (rough part) → … → camera,   X at camera depth 1 … R

where each Sᵢ's smooth part is taken only within its **core**: the microfacet normal m at Sᵢ within `θcore(α)` of the
shading normal (α the lobe's roughness; `θcore` where GGX's distribution of normals holds a fixed share, ruling M3).
K is the chain length (ruling M2: 1 in step 2a, 2 in step 2b), R the depth (ruling M5). The camera side keeps every
other term, including each smooth lobe's **tail** (normals past the core: broad, so the camera side finds it with low
variance) and anything deeper or longer.

- **Why a core:** GGX gives every normal a nonzero density, so with the whole lobe every triangle facing anywhere could
  hold a solution and no bound could prune one. Splitting the lobe by the sampled normal's angle is an exact partition
  (each term goes to one technique), like step 1's roughness split, and leaves the manifold side a compact domain.
- **The camera side drops exactly those terms**, extending step 1's recurrence. At every bounce at depth ≥ 1 that
  leaves by its rough part, a new chain starts; past it each bounce's smooth part continues it. Kept per chain length
  ℓ = 1 … K as `chainₗ` (float3 each), with every factor `throughput` takes:

      chain₁ ← throughput × f_rough / pdf        (at a bounce at depth 1 … R)
      chainₗ₊₁ ← chainₗ × f_smooth,core / pdf     (each later bounce)

  At a directional light's sample at a vertex with chain ℓ ≤ K, the sample contributes
  `(throughput × f − Σₗ chainₗ × f_smooth,core(wo, l)) × Le`, where `f_smooth,core` is the smooth part with the half
  vector of (wo, l) inside its core (0 outside). Step 1's `chainThroughput` (depth 0, every normal) is unchanged.
- **Glass in step 2b:** a refraction's core is the same test on its half vector (Walter et al. 2007's for transmission).

## The manifold connection

At a rough vertex X (depth 1 … R) raygen writes a **request**: X's position and shading frame, `wo`, its material
(index and uv: textures resolve as the hit's did), the pixel, and `throughput × (X's rough part is still to be applied)`.
A separate pass, `pt.manifold`, answers each request:

1. **Choose a tuple** (step 2a: one triangle T of a caster; step 2b: an entry and an exit triangle of one solid) with
   probability `P(T)` from a hierarchy over the casters' triangles (positions and normal cones, built at upload). A
   tuple's bound is conservative: P(T) > 0 for every tuple where some core normal maps the sun to X. The bound's shape
   (Bernstein's irradiance bound, or a solid angle times the lobe's peak) is measured (ruling M4).
2. **Draw the core normals**: m ∈ core with density p(m) (GGX's visible normals, truncated to the core and
   renormalised) at each vertex of the chain.
3. **Solve**: every point y on T (every pair on the tuple) whose half vector between `X − y` and the sun's direction
   (Snell's generalised half vector through glass) is m, with interpolated normals as the tracer shades.
4. **Weigh**, per solution, by the area integral's change of variables (Zeltner 2020 §4; the determinant of the
   constraint's Jacobian):

       contribution = Σ_solutions  throughput × f_rough(X; wo, ω) × Π f_smooth(yᵢ) × G × |cos at the sun| × E_sun × V
                                    / (P(T) × Π p(mᵢ) × |det ∂m/∂y|)

   V is the visibility of every segment (ray queries, panes multiplying their transmission as shadow rays do). The sum
   over solutions makes it exact for any number of them; a missed solution is a bias, which is what the closed forms
   below catch.
5. **Splat** the contribution into the pixel's integer sum (step 1's fixed point and stochastic rounding), which the
   next `raygenCaustics` adds and clears.

The pass is separate from raygen because raygen's size is its compile time and its spill (step 1: two more inlined
evaluations kept the caustics program out of the driver's cache). Raygen only writes requests: a few stores.

## Step 2a as designed (refines the outline above)

Working the outline into the tracer's code changed five things; the outline stays for step 2b's tuples.

- **One slope, every candidate.** The slope σ (the microfacet normal in slope space) is drawn once per request and
  caster instance, and **every** triangle of the instance that can hold a root for it is solved, so P(T) = 1 and there
  is no tuple probability to estimate. The draw is shared because it is what makes the estimate smooth: for a glint on
  a sphere exactly one triangle holds the root, and its weight `f / p(σ)` has GGX's D cancelled. Drawing a slope per
  triangle would make each one a rare hit with a large weight, the dots again. The triangles are pruned by a
  conservative cone test (a box and a cone of its vertex normals cannot hold `angle(h, n) = θσ`): ruling M4 is now
  that test's cost, measured as candidates per request; choosing tuples by a bound comes back only if that measures too
  high.
- **σ per instance, its density fixed.** σ is drawn from GGX's slope density truncated to the core, at the largest α
  the instance's smooth lobes can have (`smoothAlphaBounds`: the material's factors, a coat's widening included; a
  roughness texture only lowers α, so the core is covered). With a smooth coat over a smooth base, half the draws use
  each lobe's α (a mixture). Any density positive on every core is exact; a texture far below its factor only adds
  noise.
- **A frame per triangle.** σ is the half vector's slope in a frame from the interpolated normal and the triangle's
  first edge. Any smooth frame gives a valid change of variables on one triangle, so the per-triangle frame is exact
  and its derivative is analytic (`manifold_tests` holds the Jacobian to finite differences).
- **The same hits as the camera.** A request holds the ray that met X (origin, direction), the throughput before X's
  bounce and the pixel; the manifold pass retraces it (the same ray from the same launch index meets the same point
  with the same any-hit coin), builds X as `tracePixel` does, and traces `X → y`, so S's material, its occluders and
  its panes are the camera side's. The integrand is evaluated at the solved point and normal, not the traced hit's
  (they differ by float error, which at a mirror's α would move D far from where p(σ) cancels it).
- **Its own pipeline, added to the running mean.** `manifoldConnect` is a raygen in a small program of its own (it
  compiles apart from the caustics program, whose size step 1 measured), one thread a pixel over its requests, adding
  `c / count` to the running mean the raygen just wrote and redrawing the displayed pixel, so the frame's estimate is
  complete in the frame (a splat read by the next frame would lose the last frame's).
- **Owned materials (ruling M7).** A normal map gives a triangle many roots (one per texel's slope), which Newton from
  one start would miss: a bias. Normal-mapped smooth surfaces, skinned casters and anything not in the casters'
  gathered triangles stay with the camera side: the subtraction applies only at a material whose every draw is gathered
  and which has no normal map (a bit per material). The partition is still exact; the helmet's normal-mapped glints are
  then step 1's and the camera side's, as today.

## Tests

Closed forms first; each case fails before its code exists.

- **m1_mirror_seen** (2a): a plane mirror reflects the sun onto the floor; the camera sees that floor only after a bounce
  off a matte wall. The spot's light on the wall has a closed form (the mirror's Fresnel x the sun x the wall's albedo x
  form factors). Camera-side only is noisy; with step 2a within 1%.
- **m2_glossy_mirror** (2a): the same mirror at roughness 0.1: the spot's irradiance by numerical quadrature over GGX's
  normals (a 2-D integral, numpy), within 1%; the core and tail split checked by rendering each side alone
  (`OHAO_PT_CLASS`) against the quadrature split at `θcore`.
- **m3_sum** (2a): camera side alone (step 2 off) against step 2 on, at high spp, on m2's scene and the showcase's crop:
  means within 1% (every term counted once).
- **m4_slab_seen** (2b): step 1's glass slab caustic (`c3_slab`'s closed form), seen in a mirror: the closed form times
  the mirror's Fresnel, within 2%.
- **m5_ball_seen** (2b): `c3_sphere`'s moved glass ball, its caustic seen after a bounce, against the same numpy ray
  simulation, 5%.
- **m6_through_glass** (2b): c4's slab between the camera and step 1's glass slab caustic (`c3_slab`), now found through glass.
- **m7_showcase**: equal time against the 8192 spp reference: the glint-after-rough class (`OHAO_PT_CLASS=2`) has no
  dot; relMSE no higher than step 1's; dots no more than step 1's 7.
- **m8_repeat**: two captures in one process identical (integer splats, fixed tuple order).
- **Unchanged**: a scene without casters or without a directional light bit for bit as today; the render goldens.

## Build order

- **Step 2a** — chains of one (reflection and a smooth coat), the camera side's recurrence, requests, the pass, tuple
  choice over single triangles; m1, m2, m3, m7, m8. The dots.
- **Step 2b** — chains of two (refraction through solids), m4, m5, m6. The missing glass-seen light.

## Risks

- **A solver that misses a root is a bias.** m1, m2, m4, m5 are closed forms or an independent simulation so that
  shows; Specular Polynomials is the fallback (ruling M1).
- **Request memory**: R requests a path a frame. At 1080p, 2 requests of 32 bytes: 133 MB. Tiles or a smaller R if it
  measures too large (ruling M5).
- **Textured roughness**: a triangle's core is bounded with the roughness texture's minimum (as the casters' table is).
- **The camera side's subtraction** needs `f_smooth,core` at light samples: one more evaluation in raygen, which step 1
  measured can cost the cache. `OHAO_PIPELINE_STATS=1` and the cold/warm compile times are checked after it.

## Rulings for Frank

- **M1 (OPEN) — the solver within a tuple.** Newton from the triangle's centre (cheap; one triangle rarely holds two
  roots) or Specular Polynomials (every root by resultants). Recommendation: start with Newton and keep it only if
  m1-m5 show no missed roots; else polynomials.
- **M2 (PROVISIONAL) — chain length K.** 2 (step 2b): a glass ball is two refractions, a glint one reflection. Longer
  chains stay with the camera side.
- **M3 (OPEN) — the core, `θcore(α)`.** The angle holding a fixed share of GGX's normals: 0.9, 0.99 or 0.999, measured
  on m2 and the showcase (a larger core: fewer camera-side dots, more triangles per request). **Found while building
  2a:** a share alone leaves a mirror's tail (α 1e-4: 1% of its light past a slope of 1e-3, 0.06 degrees) as narrow as
  the lobe itself, which the camera side cannot find (m1 would read 1% dark, and its few finds would be dots). The
  core is the wider of the share's (`kManifoldCoreSlope2` 99: 0.99) and a fixed slope, 0.1 (`kManifoldCoreMinSlope2`,
  5.7 degrees: a mirror's tail past it carries α² / 0.01 of its light, 5.5e-6 at roughness 0). The slope draw is GGX's
  at the lobe's α truncated to that disc, so its weight keeps D cancelled.
- **M4 (OPEN) — the tuple bound.** Bernstein's irradiance bound or a solid angle x lobe peak, measured at equal time.
- **M5 (OPEN) — the depths R.** 2 (depths 1 and 2) recommended; measured by the class images (what the glint class
  leaves at depth 3+).
- **M6 (PROVISIONAL) — the diagnostic stays.** `OHAO_PT_CLASS` is how m7 is judged; it lives in the caustics raygen only.
- **M8 (OPEN) — requests' roulette.** A request whose casters' summed solid angle from X is under
  `kManifoldRouletteWeight` runs with probability its ratio to it (exact). 0.02 / 0.1 / 0.5 cost 70 / 60 / 46 ms of the
  showcase's frame; 0.1 kept until an equal-time measurement decides.
- **M9 (OPEN) — the default.** The pass is exact and finds light the camera side misses (a mirror's sun seen past the first
  hit), but on the showcase costs about 3x `pt.trace` and loses at equal time (its dots are normal-mapped glints, M7).
- **M7 (PROVISIONAL) — owned materials.** Normal-mapped smooth surfaces and skinned casters stay with the camera side
  in step 2 ("Step 2a as designed"). If the showcase's remaining dots turn out to be the helmet's, normal-mapped glints
  get their own step (roots per texel, as Zeltner et al. 2020's glints do).

## Licences

Every method here is built from its paper. Before anyone opens Specular Polynomials' code (`github.com/mollnn/spoly`)
or PSMS ReSTIR's, its licence is checked; GPL code is never read (as merian-quake for path guiding).

## References

- Zeltner, Georgiev, Jakob, "Specular Manifold Sampling for Rendering High-Frequency Caustics and Glints", SIGGRAPH 2020.
- Hanika, Droske, Fascione, "Manifold Next Event Estimation", EGSR 2015.
- Fan, Guo, Wang, Xiao, Zhang, Zhou, Chen, Hu, Guo, "Specular Polynomials", SIGGRAPH 2024 (arXiv 2405.13409).
- Fan, Wang, "Bernstein Bounds for Caustics", SIGGRAPH 2025 (arXiv 2504.19163).
- Hong, Duan, Wang, Yuksel, Zeltner, Lin, "Sample Space Partitioning and Spatiotemporal Resampling for Specular
  Manifold Sampling", SIGGRAPH Asia 2025.
- Walter, Marschner, Li, Torrance, "Microfacet Models for Refraction through Rough Surfaces", EGSR 2007.
