# Sub-project 23 — Path guiding: Markov chain path guiding in the path tracer

Status: written 2026-11-02 (documents only; no code), on Frank's ruling that day: "do the 2025 Alber one", after a
showcase render of Sponza at golden hour kept bright dots that no sample count removed. Judgment calls are written under
"Rulings for Frank" as **PROVISIONAL** (recommended; the design follows them) or **OPEN**. The spec is binding once Frank
accepts it.

Roadmap: row 23 of `docs/ROADMAP.md`, built now, before sub-project 13's step 2 (ragdolls). Done when the path tracer
guides its bounces by a learned light field, its images stay the same estimate (the closed forms and the unguided
tracer agree with it), and the caustic dots of the motivating scene fall to a stated bar at equal time.

The paper: Lucas Domingo Alber, Johannes Hanika, Carsten Dachsbacher, "Real-Time Markov Chain Path Guiding for Global
Illumination and Single Scattering", Proc. ACM Comput. Graph. Interact. Tech. 8(1), Article 15, May 2025,
doi:10.1145/3728296 (CC BY-NC 4.0; a local copy for reading in `C:/Users/djmax/ohao_refs/papers/alber2025.pdf`, never
committed). It builds on Dittebrandt, Schüßler, Hanika, Herholz, Dachsbacher, "Markov Chain Mixture Models for
Real-Time Direct Illumination", CGF 2023, and fits von Mises-Fisher lobes as Ruppert, Herholz, Lensch, "Robust fitting
of parallax-aware mixtures for path guiding", SIGGRAPH 2020. **Its authors' code (merian-quake) is GPL-2.0: it is not
read, copied or linked**; this is our implementation from the paper, and the parameters the paper does not state are
chosen by measurement here.

Builds on:
- **Row 17, OpenPBR** (`2026-10-29-openpbr-materials-design.md`): `OpenPBRSurface`'s `sample()`, `pdf()` (chi-square
  held by `material_tests`) and `evalLobes()`; guiding needs a material's exact density in any direction, which
  `pdf()` is.
- **Row 18, the frontier renderer** (`2026-10-30-frontier-renderer-design.md`): the one path tracer
  (`shaders/rt/path_tracer.slang`), its next-event estimation weighed by the power heuristic, Owen-scrambled Sobol in
  groups of four, emissive triangles as lights, the radiance cache's world-space hash grid (`rt/radiance_cache.slang`:
  levels by distance from the camera, keyed by the side a surface faces, integer sums so captures repeat), and the
  rule that raygen has one trace and one light and one environment shadow ray.

## Why (what the renderer does today)

The path tracer draws a bounce's direction from the BSDF alone. Light it can reach by light sampling (lights, emissive
triangles, the environment) it finds well; light that only arrives *off a surface* it finds only when a BSDF sample
happens to point there. When that light is a small bright patch, a sample that finds it is hundreds of times the
pixel's mean: a bright dot.

The motivating scene (Khronos Sponza under the physical sky at 62°, the DamagedHelmet on a plinth, chrome, gold and glass
spheres; 960x540, rendered 2026-11-02, `rt_offline`, no denoiser):

| spp | pixels over 4x their 5x5 median | the same pixels at 4x spp |
|---|---|---|
| 2048 | 11,449 | 80% of them still over 4x; their excess fell 2.4x |
| 8192 | 12,089 | — |

Each dot fades with samples but new ones appear as fast: the count does not fall. With the helmet out of the sun the dots
on the shaded floor and curtains fell from 1,079 to 386 (bare Sponza: 299): sun on the chrome helmet and on Sponza's
own glossy surfaces, reflected into the shade, is the light BSDF sampling finds by luck. It is a glossy caustic. A
denoiser would hide it (OIDN, lossy) and a clamp would drop it (biased); guiding finds it on purpose and keeps the same
estimate.

## Thesis: learn where the light comes from, keep the estimator exact

At each bounce the tracer draws its direction from a mixture: the BSDF, and a few von Mises-Fisher (vMF) lobes read from
a world-space hash grid that learns, from the paths themselves, from which directions light arrives near that point.
The ray's weight divides by the mixture's exact density given the lobes it read (one-sample MIS, balance heuristic), so
whatever the grid holds, even nonsense, the estimate stays the same: a bad grid costs noise, never bias. The grid
learns as Alber et al.'s Markov chains do (each cell a population of states that resampling, a maximum-likelihood step
and an acceptance test evolve), with two departures that this engine's rules need: it learns from the path's own
estimate instead of an irradiance cache (so it sees glossy light, which is the point), and it commits its writes
deterministically (so captures repeat).

## Goal

1. The showcase scene at 1920x1080 in `rt_offline` with guiding on and no denoiser: the dots on the shaded floor,
   curtains and plinth at or below what the unguided tracer leaves at 4x the samples, at equal time (the bar in
   "Acceptance", OPEN G8).
2. Every closed form the referee holds (`check_material.py`'s furnace and closed box, `check_analytic.py`) holds with
   guiding on, and a guided image's mean equals the unguided one's within the noise on every reference scene.
3. A guided capture repeats bit for bit in one process, and across processes within the BVH floor, as the tracer does
   today.
4. Off, nothing changes: every golden and test is bit-identical with guiding off (its default in step 1).

## Scope

In 23:
- the vMF module (`shaders/sampling/vmf.slang`): density, sampling, the mean-cosine-to-concentration fit, a CPU twin
  through slangc's C++ (as `sobol_test` and `material_tests` do), held by chi-square and integral tests;
- the guiding grid (`render/rt/path_guiding.*`, `shaders/rt/path_guiding.slang`): states, two buffers, the resampling,
  the update and the deterministic commit;
- guided bounces in the path tracer's offline profile (step 1) and realtime profile (step 2), next-event estimation's
  MIS weights against the mixture;
- `render.settings`/`render.capture` `guiding` (`off`, `on`), `ohao run --guiding`, `ohao serve --window --guiding`;
- a caustic test scene, its reference, the measurements; `render_bench` passes and proposed budgets.

Later, by need:
- guiding RT GI's rays in the deferred frame (`gi_trace.slang`: two rays a pixel at half resolution; the same grid
  could serve them; a step of row 16 if its noise asks);
- single scattering and volumetric caustics (the paper's section 4 extension): the engine has no participating media
  yet;
- the paper's dynamic-light velocity prediction (section 5, Eq. 9-10): for realtime scenes with fast emitters (the
  muzzle flash); step 2 measures whether it is needed;
- specular chains through clear glass (sun through a glass ball onto the floor): guiding learns a near-delta lobe
  poorly; manifold sampling (Zeltner et al. 2020, specular manifold sampling) is the tool, its own row if a scene needs
  it.

## The design

### The light field a state holds (the paper's Table 1)

A state is the sufficient statistics of one vMF lobe, not its parameters, so a state read at another point still means
something (Ruppert et al. 2020):

| field | meaning | encoding (32 bytes) |
|---|---|---|
| `w` | the luminance estimate of the incident light (the resampling weight) | f32 |
| `y` | `w`-weighted mean of the points the light came from (world, m) | 3 x f32 |
| `r` | `w`-weighted mean cosine between the sample's direction and the lobe's mean | f32 |
| `n` | samples in the state (to `kGuideNMax`) | u16 |
| `t` | the frame it was last updated (for the discard rule) | u16 |
| `check` | the cell's second hash (a slot reused by another cell reads as empty) | u32 |

At a point `x` the lobe is `mu = normalize(y / w - x)`, mean cosine `rbar = (n^2 r/w + n_p r_p) / (n^2 + n_p)` with the
prior `r_p = 0`, `n_p = 1 / |y/w - x|^2` (Algorithm 2, line 7: a far, small light gets a sharp lobe quickly), and
concentration `kappa = (3 rbar - rbar^3) / (1 - rbar^2)` (Banerjee et al. 2005), capped at `kGuideKappaMax`. The sun and
the sky have no point: a sample whose path escaped is stored as a point `kGuideFar` (10 km) along its direction, as the
paper does.

### The grid

Two hash grids of states, as the paper's (section 4, "Hash Grid"), keyed like the radiance cache so they agree on what a
cell is:
- **adaptive:** levels by distance from the camera (`cacheLevel`, a voxel 0.05 m within 1 m, doubling), the key hashing
  the voxel, the level and the side the surface faces (`cacheSide`; the paper's cube-map index of the normal); a read
  goes to a coarser level with probability falling geometrically (OPEN G5: 1/2 per level), so a cell with no states
  yet finds its parent's;
- **static:** a fixed voxel (OPEN G5: 1 m), no normal in the key; a state is taken only if its lobe's mean is within
  `kGuideStaticCos` of the normal's hemisphere (the paper's normal threshold).

A read picks the adaptive grid with probability `kGuideAdaptive` 0.7 (the paper's) and jitters the point by a voxel's
trilinear weights first (the paper's stochastic interpolation: a vertex with probability by its weight), so states
spread between neighbouring cells. A slot is `hash(key) mod capacity`; one state a slot (the paper found 8 a vertex no
better); `check` detects a collision, which reads as empty (Algorithm 1, line 7). Capacity 2^20 states a grid (32 MB),
OPEN G5.

**Two buffers.** Frame N reads the states frame N-1 left (`read`) and writes frame N's (`write`, a copy of `read` at the
frame's start). Reads never see this frame's writes. `resetAccumulation` clears both (a capture starts from an empty
grid, so it repeats from any frame and any host; the radiance cache does the same).

### A bounce (Algorithms 1 and 2)

At a vertex the tracer may guide (OPEN G4: an opaque surface's reflection: no transmission, no subsurface, not
thin-walled; a walk's exit and a glass crossing never), before next-event estimation:

1. **Resample** (Algorithm 1): read `kGuideCandidates` (OPEN G5: 5, the paper's) states from the grid at the hit point,
   each by its own jittered read; keep each one's lobe `(mu_i, kappa_i)` and weight `w_i` (0 if empty, collided or
   facing away); `W = sum w_i`; pick state `s` with probability `w_s / W` (weighted reservoir, one pass). Random numbers
   for the reads and the pick come from PCG seeded by the pixel, the sample index and the depth (they choose *which
   density* to use, not where a sample lands; the Sobol dimensions stay for the direction).
2. **The density.** With `g` the guiding probability (OPEN G4: 0.5 when `W > 0`, else 0):
   `q(wi) = (1 - g) p_bsdf(wi) + g sum_i (w_i / W) vMF(wi | mu_i, kappa_i)`.
   This is the exact density the next step draws from, given the states read: the one-sample MIS estimator with the
   balance heuristic over the BSDF and the candidates. The paper divides by the unweighted sum over the accessed
   states (stochastic MIS, West et al. 2020, Eq. 6-7); the weighted form here is its conditional density under the
   resampling probabilities, needs no reasoning about the chains' equilibrium, and is unbiased for any grid content:
   the grid depends only on earlier frames, so each frame's estimate is unbiased given it, hence unbiased.
3. **Next-event estimation** weighs a light or environment sample against `q(wi)` instead of `p_bsdf(wi)`
   (the power heuristic, as today).
4. **The direction.** A coin (Sobol, a new group per depth: `kGuideGroup + d`, so the existing groups' sequences are
   untouched) chooses: the BSDF (its group `3 + 3d`, as today) or a lobe `i` by `w_i / W` and a vMF sample (the
   group's other dimensions; Jakob 2012's stable form). The weight is `f(wi) cos / q(wi)`: the BSDF branch's
   `s.weight * s.pdf / q`, the lobe branch's `evalLobes` over `q`. `lastPdf = q(wi)` for the next hit's MIS against
   light sampling.

A vertex that does not guide runs exactly today's code.

### Learning (Algorithm 3, from the path)

The paper estimates the incident radiance a sample brought (`L(hit_next, -wi)`, Eq. 7) from an irradiance cache that
assumes Lambertian surfaces, so a chain can train at a vertex without the rest of the path. That cache cannot see a
glossy glint (it holds what a surface reflects *on average*), and the glint is this sub-project's reason. Here the
estimate comes from the path itself (PROVISIONAL G1): at a guided vertex `k` the tracer records the radiance the path
had gathered (`R_k`, after the vertex's own light sampling) and the throughput arriving there (`beta_k`); when the path
ends, `(R_end - R_k) / beta_k` is the one-sample estimate of `f cos L_i / q` along the drawn direction, the paper's
`<L_i>` times `f cos / q`, and `f_mc` is its luminance. Its light sampling at later vertices is included with its MIS
weights, so a chain learns most where light sampling helps least.

Raygen does not hold these across its traces (every word raygen keeps across a trace is spilled by every pixel): it
writes each guided vertex's **proposal** (`R_k`, `beta_k`, the drawn direction's end point, the vertex, the resampled
state `s`, `W / N_mc`) to a buffer, at most `kGuideTrainVertices` (OPEN G5: 2, the first two guided vertices) a pixel,
and its final `R_end` beside them. A compute pass (`pt.guide.update`) then does, for each proposal, Algorithm 3:
- `f_mc` as above; the **acceptance** `min(f_mc / (W / N_mc), 1)` against a PCG coin (Eq. 8);
- if accepted, the **maximum-likelihood step**: `n = min(n + 1, kGuideNMax)`, `alpha = max(1/n, kGuideAlphaMin)`,
  `w`, `y` and `r` blended toward `f_mc`, `f_mc x_next` and `f_mc dot(dir, mu)` by `alpha` (lines 4-9), `t` = the
  frame;
- the **discard rule** (section 5): a state whose `t` is not this frame, whose sample pointed within its lobe
  (`dot(dir, mu) > rbar`) and found far less (`f_mc < kGuideDiscard w`, OPEN G5: 1/16) is written with `w = 0`, so a
  light that went away stops drawing samples;
- the write goes to a **new** slot: the cell of a fresh jittered read at the vertex, as the paper writes (so states
  spread).

**Deterministic commit** (PROVISIONAL G2). The paper writes states in place without synchronization, accepting that
races perturb the chains (their equilibrium absorbs it and the estimator does not care). Here a race would make two
captures differ, which every test of this renderer assumes they do not. So writes happen in two passes: every
accepted proposal does `InterlockedMax` of a 64-bit key `(hash(frame, proposal) << 32 | proposal index)` on its target
slot's priority word; then the proposal whose key won writes its state to `write`. One write a slot a frame, chosen by
a hash, is the paper's "a random last writer wins", repeatable.

### What it costs (estimates to be replaced by measurements)

Per guided vertex: 5 hashed reads (32 bytes each), a 5-term vMF sum at the drawn direction and at each light sample,
one proposal written. Per frame: the update pass over at most 2 proposals a pixel, the commit, a 32 MB copy. Memory at
1080p: two grids (64 MB), the priority words (8 MB), proposals (2 a pixel x 48 bytes: about 200 MB; OPEN G5 if that is
too much). The paper measured 13.1 to 15.3 ms (+17%) for its whole pipeline on an RX 7900 XTX.

## The agent-facing surface

- `render.settings {guiding: off | on}` (default off in step 1), `render.capture {guiding}`, `ohao run --guiding on`,
  `ohao serve --window --guiding on`; `render.stats` reports the guiding passes' timings and, when guiding ran,
  `guiding: {states, accepted, discarded}` for the last frame.
- `OHAO_GUIDE_DEBUG=1` adds a capture `aov` of the learned lobe at each pixel's first hit (mean direction as colour,
  sharpness as value: the paper's Fig. 3 right), for looking at what it learned.

## Determinism

- Guiding off: bit-identical to today (no draw, buffer or branch of the existing path changes).
- Guiding on: reads from last frame's buffer, PCG and Sobol seeded as above, integer-free but order-free commits (the
  64-bit max), a fixed proposal order: two captures in one process are the same bits; across processes the BVH floor
  (a ray on a shared edge may take either triangle) applies as today, and a flipped hit can change what a chain learns,
  so the floor may spread further than today's few pixels. Measured in step 1 and stated, never hidden.

## Performance budgets

Proposals, each `pending` until Frank rules (as every budget):
- `helmet_rt_1080p` and `play_rt_1080p` with guiding on: `pt.trace` within 1.3x of guiding off; `pt.guide.update` plus
  the commit 0.5 ms;
- the offline showcase: equal-time comparisons, not budgets (the acceptance below).

## Test plan

Test numbers continue the frontier's sequence as `g1`... (`tests/renderer/`):
- **g1, the vMF module** (`vmf_test`, CPU, slangc C++ twin): `sample()` against `pdf()` by chi-square (64x64 bins) at
  kappa 0.1, 5, 100, 10^4; the density integrates to 1 within 0.1%; the fit `kappa(rbar)` inverts the vMF's mean cosine
  within 1% over rbar 0.05-0.999; a state built from N samples of a known lobe recovers its mean and kappa.
- **g2, the estimate is the same** (`check_material.py --guiding`, `check_analytic.py --guiding`): the white furnace
  within 1%, the closed box within its closed form, every analytic case within 5%, with guiding on; and on Cornell, the
  helmet room and the showcase at 4096 spp: guided and unguided means within 1% overall and within the noise per 32x32
  block (the bar computed from each image's own variance, not a tolerance chosen to pass).
- **g3, it learns**: a caustic scene (`assets/scenes/caustics.scene.json`: a sun through a roof slot onto a chrome
  sphere, the floor around it in shade); after 256 frames the first hit's learned lobe on the shaded floor points at the
  sphere within its own spread; the guided image at N spp has a lower RMSE than the unguided at N against a reference
  (unguided, 2^18 spp at 160x90).
- **g4, determinism**: two guided captures in one process equal bit for bit; guiding off equal to the build before it
  (every golden unchanged).
- **g5, the dots**: the showcase at 960x540: pixels over 4x their 5x5 median on the shaded floor, curtains and plinth,
  guided at N against unguided at 4N, at equal time (the bar, G8).
- Validation clean with guiding on (`check_validation.py` gains a guided path-traced capture).

## Acceptance (from Frank)

Look at the showcase guided and unguided at equal time, full size; the dots that matter are the ones you see.

## Done criteria

g1-g5 pass; every gate green; the showcase shown to Frank at 1080p; a change record per step.

## Build order

1. **Step 1, offline:** the vMF module and g1; the grid, resampling, mixture density, next-event MIS, proposals, the
   update and the commit in the offline profile; `guiding` in settings, capture, CLI; g2-g5; the measurements.
2. **Step 2, realtime and budgets:** the realtime profile (guiding the first vertex before the radiance cache ends the
   path, the cache's cone rule for a guided sample), `ohao serve --window --guiding`, `render_bench` passes and
   proposed budgets, the default (G6), the velocity question.

## Risks

- **Spill.** Raygen keeps what it needs across its trace for every pixel; the candidates live only between the resample
  and the draw (no trace in between), the proposal goes to memory. `OHAO_PIPELINE_STATS=1` reports raygen's stack
  before and after; a growth that shows in `pt.trace` with guiding off is a defect.
- **Learning from the path is noisier** than the paper's cache at long paths; that only slows learning (the estimate
  stays exact). If the chains do not learn the motivating glint, the fallback is the paper's cache for vertices past
  the first, ours for the first.
- **Many lights**: the paper's own limit (section 7): with 5 candidates, scenes with many comparable sources are guided
  toward the strongest. Next-event estimation still covers lights; guiding is for what it cannot reach.
- **The BVH floor** may spread through learning (Determinism).

## Review focus

The density's exactness (every branch of `q`, the BSDF branch's weight, a vMF sample below the surface), the MIS with
light sampling, the Sobol groups untouched when off, the commit's determinism.

## Rulings for Frank

- **G1 (PROVISIONAL) — learn from the path, not an irradiance cache.** The paper's cache is Lambertian and cannot see
  the glossy glint this is for. The path's own estimate, read back after the path ends, is unbiased and sees it.
- **G2 (PROVISIONAL) — deterministic commits.** Two buffers and a hashed winner per slot, instead of racy in-place
  writes, so captures repeat.
- **G3 (PROVISIONAL) — the exact mixture density.** One-sample MIS (balance) over the BSDF and the read lobes with
  their resampling probabilities, rather than the paper's stochastic MIS sum; unbiased for any grid.
- **G4 (OPEN) — where and how much to guide.** Opaque reflection vertices only; guiding probability 0.5 when any state
  was read. Recommendation: as written; measured in step 1 (0.3 and 0.7 too).
- **G5 (OPEN) — the parameters the paper leaves out** (its code is GPL and not read): 5 candidates, 2^20 states a grid,
  a coarser level with probability 1/2 per level, a 1 m static voxel, `kGuideNMax` 64, `kGuideAlphaMin` 0.02, the
  discard ratio 1/16, `kGuideKappaMax` 10^4, 2 trained vertices a pixel. Recommendation: start there, tune by g3 and g5,
  record what was measured.
- **G6 (OPEN) — the default.** Off in step 1. Recommendation: on for `rt_offline` once g2-g5 pass (step 2), off for the
  realtime profile until its budget is ruled.
- **G7 (PROVISIONAL) — the grid starts empty at every reset**, like the radiance cache: a capture is the same from any
  frame and any host.
- **G8 (OPEN) — the bar for "no dots".** Recommendation: the guided showcase at N spp has no more pixels over 4x their
  5x5 median on the shaded floor, curtains and plinth than the unguided at 4N, at equal or less time.
- **G9 (OPEN) — the realtime extras** (velocity prediction, a firefly filter after guiding): measured in step 2, built
  only if a scene shows the need.
