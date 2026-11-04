# Caustics step 2a (manifold connections, chains of one) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every term `sun → S (a smooth reflection, its microfacet normal in the lobe's core) → X (rough reflection)
→ … → camera` with X at camera depth 1 or 2 is found by solving for S's point, exactly, and the camera side drops
exactly those terms, so the showcase's glint-after-rough dots go and no light is added or lost.

**Architecture:** The caustics raygen keeps `chainOne` (the throughput of the owned term since X's rough bounce) and
subtracts `chainOne × f_smooth,core × E` at a sun sample at S; at each eligible X it writes a request (the ray that met
X, the throughput before X's bounce). A compute pass gathers the casters' triangles into world space with clusters and
normal cones. A new raygen, `manifoldConnect`, in its own small ray tracing pipeline, retraces each request's ray to X,
draws one microfacet slope per caster instance, solves every candidate triangle for the points whose half vector is that
slope (Newton, analytic Jacobian), connects them, and adds the sum to the pixel's running mean.

**Tech Stack:** C++20, Vulkan 1.3 ray tracing pipeline and compute, Slang (pinned slangc, its C++ target for the CPU
twin), Python harnesses, CTest.

**Spec:** `docs/specs/2026-11-04-caustics-step2-design.md` ("The terms step 2 owns", "The manifold connection",
"Step 2a as designed", "Tests", "Rulings").

## Global Constraints

- Build: `C:/Users/djmax/scoop/shims/cmake --build build --config Release -j6`, never more than -j6, never two builds
  at once; never build or edit while a gate or test runs. One GPU job at a time.
- Iterate with `python tools/check.py --changed`; the full `python tools/check.py` closes the step.
- **The default raygen (`raygen`, `tracePixel<false>`) is untouched**: every golden unchanged. New camera-side code is
  under `CAUSTICS` only; the manifold is its own entry and pipeline.
- Offline profile only (`rt_offline`); the pass runs only when caustics run and some caster instance is owned.
- No float atomics in shaders (`reflect_tests` lints it).
- `path_tracer.slang` and `openpbr.slang` are LF; check every file's line endings before editing.
- Engine code logs through `OHAO_LOG`. Commits local only, `type(scope): what`, body says why, ending with
  `Claude-Session: https://claude.ai/code/session_01X8FoR5xK1s65vsfTgcomek`.
- Layout constants (`shaders/shared/layout.json`, `python tools/gen_layout.py`): `kManifoldDepths` 2 (ruling M5),
  `kManifoldCoreSlope2` 99 (ruling M3: the core holds 0.99 of GGX's normals, `s² / (1 + s²)`), `kManifoldCluster` 32.

## The estimator (what every task implements a piece of)

For a request at X (position from the retraced hit, `p = X + ng × 1e-3`, frame and material as `tracePixel` builds
them), a directional light `l` (each in the casters' table, irradiance `E`), and a caster instance `I` with smooth lobe
roughness bounds `αb`, `αc` (0 when that lobe cannot be smooth):

1. Draw σ in the slope plane from `p(σ) = ½ pb(σ) + ½ pc(σ)` (one term when one lobe), `pk` GGX's slope density at
   `αk` truncated to `|σ| ≤ s αk` (`s² = kManifoldCoreSlope2`) and renormalised by `s² / (1 + s²)`.
2. For every triangle of `I` whose material is owned, in a cluster and group that pass the cone test: Newton on
   `σ(u, v) = σ*`, from the centroid, at most 12 iterations; a root counts when it converged and `u ≥ 0, v ≥ 0, u + v ≤ 1`.
   `σ(u, v)`: `y = p0 + u e1 + v e2`, `ω = normalize(p − y)`, `H = ω + L`, `N = n0 + u a1 + v a2` (world normals by the
   inverse transpose, unnormalised), `n = ±normalize(N)` turned to the side the triangle shows `p`, `t = normalize(c −
   (c·n) n)` with `c = normalize(e1)`, `b = n × t`, `σ = (H·t / H·n, H·b / H·n)`.
3. Per root y: trace `p → y` with the bounce ray's mask; the hit must be within `1e-4 × |y − p| + 1e-5` of y (else
   occluded). S's material from that hit; its shading normal replaced by `n(y)` (anisotropic tangent re-projected); the
   material must be owned. `wo_S` toward p, `wi_S = L`; require `dot(L, ng_S) > 0`, `dot(ω, ng_X) > 0`, `V = visible(pS,
   L)` (panes multiply `T`).
4. `c = β × f_rough,X(wo, ω) × f_smooth,core,S(wo_S, L) × E × T × |ng_S·ω| / |y − p|² × |e1 × e2| / (|det J| × p(σ*))`
   with `J = ∂σ/∂(u, v)`. Summed over roots, instances, lights and the pixel's requests, added to the running mean as
   `acc += c / count`.

The camera side drops the same terms: at a bounce at depth 1..`kManifoldDepths` that is a reflection (not a crossing,
not from inside a solid, not a walk's exit, the next vertex's light sample within the bounce limit), `chainOne =
throughput × f_rough(wo, wi) / pdf` (else 0); `chainOne` takes every factor `throughput` takes until the next bounce
resets it; at the next vertex a directional light's sample subtracts `chainOne × f_smooth,core(wo, L) × le` when that
vertex's material is owned.

**Owned** (a material bit the host computes): no normal map, and every draw using it is a static, non-skinned,
non-emitter caster. Everything else stays with the camera side (ruling M7).

## Review Focus

1. **The two sides agree on eligibility.** Request written ⇔ `chainOne` set (same conditions in one place). Task 4
   `m0_split_only` and Task 6 `m3_sum` catch a mismatch as a bias.
2. **Every root found.** Task 2's brute-force comparison on a sphere and a flat mirror; Task 6's closed forms.
3. **The Jacobian is the map's.** Task 2 against finite differences in double.
4. **Unchanged.** A scene with no owned caster or no sun: bit for bit as caustics step 1 (Task 4 `m0_unchanged`), the
   render goldens.

---

### Task 1: OpenPBR's core-limited smooth reflection and lobe bounds

**Files:** Modify `shaders/material/openpbr.slang`, `tools/material/openpbr_kernel.slang`,
`tools/material/openpbr_cpu.hpp`; Test `tests/material/material_tests.cpp` (`t7_*`).

**Interfaces (on `OpenPBRSurface`):**
- `float3 evalSmoothCore(float3 wo, float3 wi, OpenPBRTables t, float cut, float slope2)`: `evalParts`' smooth part for
  a reflection (`wo.z > 0`, `wi.z > 0`, not a thin wall's back), each smooth lobe kept only when the half vector's
  stretched slope `(m.x / αx)² + (m.y / αy)²` over `m.z²` is at most `slope2` (its own α: the base's `alphaXY`, the
  coat's); 0 for anything else.
- `evalLobesSmooth` gains `out float3 smoothCore` (the same, from the same evaluation; raygen calls it once).
- `float2 smoothAlphaBounds(float cut)`: per lobe (base, coat) the largest α the lobe can have while smooth (its
  `alphaXY`'s larger component, at `min(rough, cut)`), 0 when it cannot be smooth.
- CPU twin: `Op::SmoothCore = 17` (r0), `Op::AlphaBounds = 18` (r0.xy).

- [ ] Write failing tests: `t7_core_le_smooth` (≤ the smooth part per channel, the materials of `t6_parts_sum`);
  `t7_core_whole` (slope2 1e12 equals the smooth part for reflections, 0 for refractions); `t7_core_indicator` (a metal
  at roughness 0.1: inside the slope equal to the smooth part, outside 0, on a grid of half vectors); `t7_alpha_bounds`.
- [ ] Implement; `material_tests` all pass; `openpbr_tables --check` passes.
- [ ] Commit `feat(material): the smooth part's core and its lobes' alpha bounds (caustics 2a T1)`.

### Task 2: The manifold module and its CPU twin

**Files:** Create `shaders/rt/manifold.slang` (module), `tools/caustics/manifold_kernel.slang`,
`tools/caustics/manifold_cpu.{hpp,cpp}`, `tools/caustics/CMakeLists.txt` (as `tools/material`), `tests/renderer/
manifold_tests.cpp` (`LABEL quick`); rules in `tools/check_changed.py`.

**Interfaces (Slang, pure functions):**
- `float2 manifoldDrawSlope(float2 alphas, float slope2, float3 u)` and `float manifoldSlopeDensity(float2 alphas, float
  slope2, float2 sigma)`.
- `ManifoldTri` (p0, e1, e2, n0, a1, a2); `bool manifoldSigma(ManifoldTri, float3 p, float3 L, float2 uv, out float2
  sigma, out float2x2 J, out float3 y, out float3 n)` (false where the map is undefined: `H·n ≤ 0`, degenerate frame).
- `bool manifoldSolve(ManifoldTri, float3 p, float3 L, float2 target, float tol, out float2 uv)`.
- `bool manifoldConeTest(float3 boxMin, float3 boxMax, float3 axis, float cosHalf, float3 p, float3 L, float thetaSigma)`:
  false only if no point of the box with a normal in the cone can have `angle(h, n) = thetaSigma`.

- [ ] Failing tests: `t8_jacobian` (J against central differences in double, random triangles, < 1e-3 relative);
  `t8_roots_sphere` (a 96x48 sphere, 200 random p, L, σ: Newton over every triangle against a brute-force root search
  on a 64x64 grid per triangle refined by bisection: the same root set); `t8_roots_mirror` (a flat quad: exactly one
  root, at the mirror point for σ = 0); `t8_slope_density` (chi-square of the draw against the density; the density
  integrates to 1); `t8_cone_conservative` (random boxes and cones holding a root are never rejected).
- [ ] Implement; `manifold_tests` passes.
- [ ] Commit `feat(caustics): the manifold module: slopes, the map, Newton, cones (2a T2)`.

### Task 3: The casters' triangles on the GPU

**Files:** Create `ohao/render/rt/manifold_casters.{hpp,cpp}`, `shaders/rt/manifold_gather.slang` (compute: world
triangles, cluster and group bounds and cones, per instance alpha bounds by `InterlockedMax` on float bits),
`tests/renderer/caustic_casters_test.cpp` additions (owned materials). Modify `caustic_pass.*` (owns it),
`pass_registry.hpp` (`pt.manifold.gather`, `pt.manifold`).

**Interfaces:** `ManifoldCasters::setScene(const RenderScene&, float cut)` (instances, owned material bits, a version
that changes when draws, transforms or materials do); `recordGather(cmd)` (only when the version changed);
buffers: triangles (18 floats + material), clusters and groups (box, cone axis, cos), instances (first group, groups,
alpha bounds), owned bits.

- [ ] Failing CPU tests: `owned_excludes_normal_maps`, `owned_excludes_skinned`, `owned_needs_smooth`.
- [ ] GPU check in `check_caustics.py` (`m0_gather`): the gathered triangles of the showcase's glass ball against the
  sphere's analytic positions (every vertex within 1e-5 of radius × scale), every normal inside its cluster's cone.
- [ ] Commit `feat(caustics): the casters' triangles, clusters and cones for the manifold (2a T3)`.

### Task 4: The camera side (`chainOne`, the subtraction, requests)

**Files:** Modify `shaders/rt/path_tracer.slang` (CAUSTICS only), `ohao/render/rt/path_tracer_render.cpp`,
`ohao/render/rt/path_tracer.hpp` (the request buffer: `kManifoldDepths` slots a pixel, 48 bytes each), layout.

- [ ] `OHAO_PT_CLASS` widens to 8 bits; class 128 = the manifold's light.
- [ ] `OHAO_MANIFOLD_SPLIT_ONLY=1`: subtract without the pass (`m0_split_only`: the showcase's class 2 falls, the
  rest of the image within noise of step 1's).
- [ ] `m0_unchanged`: caustics' `cornell` and a scene without owned casters bit for bit as before the change.
- [ ] Commit `feat(caustics): the camera side's chains of one and the manifold's requests (2a T4)`.

### Task 5: `manifoldConnect`

**Files:** Modify `shaders/rt/path_tracer.slang` (the entry), `ohao/render/rt/path_tracer_pipeline.cpp` (a third
program, `path_tracer_manifold`: the manifold raygen, the miss and hit groups), `path_tracer_render.cpp` (dispatch
after `raygenCaustics`, barriers, the display rewritten), stats (requests, candidates, roots, connections).

- [ ] Commit `feat(caustics): manifold connections from the sun's smooth reflections (2a T5)`.

### Task 6: Closed forms and the split's sum

**Files:** `tests/renderer/check_caustics.py` (`--manifold`: m1, m2, m3, m8), `assets/scenes/` test scenes,
`tests/renderer/manifold_tests.cpp` (`m2_reference`: the glossy mirror's floor irradiance by quadrature through the
CPU twin, written as JSON).

- [ ] **m1_mirror_seen**: a white mirror (metal, roughness 0) reflects the sun onto a white Lambertian floor that the
  camera sees in a second mirror; `--bounces 2`, sky off. Closed form in the spot: `F2 ρ/π E (cos_sun + cos_r)`.
  Without step 2 the spot is missing; with it within 1%.
- [ ] **m2_glossy_mirror**: the first mirror at roughness 0.1; the floor's irradiance from `m2_reference`; within 1%;
  the class images split at the core against the quadrature split.
- [ ] **m3_sum**: step 2a off (`OHAO_MANIFOLD_OFF=1`) at high spp against on: means within 1% on m2's scene and on the
  showcase's crop.
- [ ] **m8_repeat**: two captures in one process identical.

### Task 7: The showcase, measurements, docs, close

- [ ] m7: equal time against the 8192 spp reference: class 2 has no dot, relMSE no higher than step 1's, dots ≤ 7.
- [ ] Measure M1 (missed roots: m1-m3), M3 (0.9 / 0.99 / 0.999 on the showcase), M5 (class 2 left at depth 3+), the
  pass's time and the raygen stack (`OHAO_PIPELINE_STATS=1`), cold compile times.
- [ ] Change record `docs/changes/YYYY-MM-DD-caustics-step2a.md`, AGENTS.md's caustics paragraph, ROADMAP row, the
  spec "as built". Full `python tools/check.py`.
