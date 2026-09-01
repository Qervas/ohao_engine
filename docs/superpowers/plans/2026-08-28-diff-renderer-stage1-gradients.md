# Stage 1 — Interior Gradients

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the wavefront integrator differentiable — hand-derived Lambert and GGX adjoints, PRB replay, texture scatter — gated on common-random-number finite differences.

**Architecture:** Path Replay Backpropagation. The backward pass is a second forward traversal, not a tape: `O(1)` memory for roughly `2x` compute. At each vertex the forward computes `L = Le + f(theta) * L_i / pdf`; given an adjoint `dL` arriving there, backward does exactly two things — scatter `dL * (df/dtheta) * L_i / pdf` into the gradient arena, and propagate `dL_next = dL * f / pdf`. **The only genuinely new mathematics is `df/dtheta` per BSDF.**

**Tech Stack:** Vulkan 1.3, `VK_KHR_ray_query`, `shaderBufferFloat32AtomicAdd`, GLSL compute, C++20.

**Spec:** `docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md` (§6.1–6.3 are the contract this plan implements)

---

## Global Constraints

- **Hand-written GLSL adjoints.** No autodiff framework. The spec chose this so the mechanism stays inspectable; Slang remains an escape hatch only if hand-derivation becomes the bottleneck, and only behind a harness that already passes.
- **Detached sampling.** Sampled directions are not differentiated. Neither is the ray query (visibility is discrete), Russian roulette, or lobe selection. Differentiable: BSDF evaluation, texture reads, emission, throughput products, MIS weights.
- **No new Vulkan features or extensions** beyond `VK_KHR_ray_query` and `shaderBufferFloat32AtomicAdd`. **Never image atomics** — buffer float atomics only.
- **A stage records WORK, never ORDERING.** `WavefrontStage` contains no barriers; `WavefrontLoop` owns them all. Any caller-owned buffer a stage WRITES goes through `record()`'s `extraBarrierBuffers`; read-only buffers must not.
- **Editing a shader can break the documentation build.** `site/content/**/*.md` cites shader source by exact substring and `cite.py` raises when it is gone. Grep `site/content/` for citations of lines you touch and run `python site/tools/generate_tree.py` (must exit 0). This slipped through twice in Stage 0b-2b and no C++ test detects it.
- **Check counts are descriptions, never targets.** Count before, count after, report both. The only failure is a check that disappears, goes silent, or gets weaker. The probe currently prints 39 `OK:` lines.

## Inherited hazards — read all of these before Task 1

**1. Synchronization validation is MEASURED BLIND here.** With every compute-side memory barrier in the fused loop disabled, all checks pass across three runs with zero `SYNC-` diagnostics. The only barrier whose absence anything detects is `COMPUTE_SHADER → DRAW_INDIRECT`. **A passing test suite is not evidence about barriers.** Reading them is the only guard.

**2. The seed invariant covers the PATH, not the FILM.** Verified at the close of Stage 0b-2b: `kDrawsPerBounce` is branch-independent (the lobe draw is unconditional and both environment draws are taken *before* the miss guard, so a hit and a miss consume identical stream positions), the fast-forward reads `bounce` from path state alone, and rejected-sample substitution is deterministic. **But at >1 spp several paths of one pixel `atomicAdd` the same three film floats in one dispatch, and float addition is not associative** — so the film is not a pure function of `(pixel, sampleIndex, iterationSeed)`. Nothing observes this today because every probe runs 1 spp and check 32 asserts `pixelHits == 1` as a hard failure.

**This is the hazard most likely to cost this stage a day.** A PRB forward/backward mismatch at multi-spp would present as a gradient that is *almost* right and irreproducible between runs — which reads as a backward-pass bug while being an accumulation-order artefact. **Decide before writing the backward pass:** replay at 1 spp per dispatch, accumulate per-path and reduce deterministically, or document a tolerance covering atomic reordering.

**3. The recurring defect class: a check whose expected value derives from the same constant, shader or helper as the measured value cannot fail, while looking rigorous.** Stage 0b-2b produced five instances, every one found by review rather than by a test:
- a furnace bound that **passed its own perturbation** (a term it relied on never fired in that configuration);
- a report that **claimed a verdict for a check that never ran**, because check ordering made the inference natural;
- a derivation **never reconciled against the code it described** — whose fix round then found a **real production shader bug** hiding 0.056 sigma under a noise floor;
- a check header claiming a rejection it **could not perform**, because a sink slot and the value under test read the same local variable;
- a statistical gate **never observed to be the decisive rejector** until a localised perturbation was constructed for it.

**Every gate in this stage needs an oracle derived independently of the code path it measures, and every one needs a demonstrated failure.** Treat a bound that survives your own perturbation as evidence the bound is wrong. If your oracle shares a variable, a constant, or a helper with what it measures, it is not an oracle.

**4. Constants transcribed across the GLSL/C++ boundary must be tied, not commented.** Stage 0b-2b built `checkNeeStrideTie`, `checkScatterPushSizeTie`, `checkWfScatterSinkLayoutTie` and `checkParityRefConstantsTie` — runtime source-parsers, comment-stripped — because "naming each other in a comment was the whole of the tie and was not enough." Any constant this stage transcribes gets the same treatment, or an explicit statement of why it is covered by measurement instead.

**5. Assertions quantified over a set that can be empty pass vacuously.** Prefer quantifying over all `kCapacity` entries and asserting the live count separately.

## The one thing that makes gradients silently wrong

**The backward kernel must walk the identical path, consuming the identical RNG values in the identical order.** Divergence by a single RNG call means the replayed path is a different path and every gradient is wrong — *no crash, no NaN, no diagnostic*.

Under a wavefront design this sharpens rather than softens: path state crosses a dispatch boundary between bounces, so nothing may rely on registers surviving. Every stage already reconstructs its RNG from `(pixel, sampleIndex, bounce)` rather than carrying it, which is exactly what makes replay possible.

`drawCount()` is the tripwire, and the wavefront makes it **assertable per stage rather than only per path** — forward and backward must consume the same number of draws at every bounce.

Per spec §6.2, **the traversal is one piece of source, included twice**, with a per-vertex hook the includer defines. Divergence is made structurally impossible rather than prevented by discipline. Task 1 establishes this before any gradient exists.

---

### Task 1: Replay equivalence, before any gradient exists

**Files:** Create `shaders/includes/diff/traverse.glsl`; modify `shaders/diff/wf_scatter.comp`; create `shaders/diff/wf_scatter_replay.comp`; modify `tests/diff/gpu_probe_context.{hpp,cpp}`, `tests/diff/diff_gpu_probe.cpp`.

Extract the scatter stage's traversal into one included source with a `VERTEX_HOOK` the includer defines, and build a second instantiation that replays. **No adjoint, no gradient, no arena.** The deliverable is proof that two instantiations walk the same path.

- [ ] **Step 1: Write the failing check first.** Assert that for every path and every bounce, the replay stage consumes **exactly** the same RNG draws — values *and* `drawCount` — as the forward stage, and reconstructs the same origin, direction, throughput and hit. The oracle is the forward run's own recorded stream, read back independently; the replay must not be handed anything the forward stage did not write to path state.
- [ ] **Step 2: Run it and confirm it FAILS** (no replay stage yet). Paste it.
- [ ] **Step 3: Extract `traverse.glsl`** and re-point `wf_scatter.comp` at it. **Every existing check must still pass, untouched** — this is a pure extraction, and checks 14/17's exact `== 0.0625`, the survival check, and checks 20–34 are the proof.
- [ ] **Step 4: Add the replay instantiation.**
- [ ] **Step 5: Verify.** Then **demonstrate the check can fail**: insert one extra RNG draw in the replay hook, confirm the check rejects, restore. **Paste both.** This is the single most important demonstration in the stage — it is the failure mode that produces silently wrong gradients.
- [ ] **Step 6:** Resolve inherited hazard 2 explicitly. State in the code which of the three options you took and why. **Nothing will test it.**
- [ ] **Step 7: Commit.**

---

### Task 2: The Lambert albedo adjoint

**Files:** Create `shaders/includes/diff/bsdf_adjoint.glsl`; modify `shaders/diff/wf_scatter_replay.comp`; modify the probe.

For Lambert, `f = albedo / pi`, so `df/dalbedo = 1 / pi`. The scatter is `dL/dalbedo += dL * (1/pi) * L_i / pdf`.

- [ ] **Step 1: Write the failing check first — a CRN finite difference.** Perturb albedo by `+h` and `-h`, render both with **identical** `(pixel, sampleIndex, iterationSeed)`, and compare `(L(+h) - L(-h)) / 2h` against the analytic gradient. **State the `h` you chose and derive it** — too large and truncation error dominates, too small and cancellation does. Report both error terms and where their sum is minimised.
- [ ] **Step 2: Confirm it FAILS.** Paste it.
- [ ] **Step 3: Implement**, scattering into the gradient arena with `atomicAdd`. The arena is caller-owned and written by a stage: route it through `extraBarrierBuffers`.
- [ ] **Step 4: Verify**, and assert the gradient is **exactly zero** for a parameter the scene does not depend on — a null test that a scatter into the wrong arena block would fail.
- [ ] **Step 5: Demonstrate failure** — scale the analytic adjoint by 1.01, confirm the FD gate rejects, restore. Paste both.
- [ ] **Step 6: Commit.**

---

### Task 3: The GGX adjoints — roughness and metallic

**Files:** Modify `shaders/includes/diff/bsdf_adjoint.glsl`, the replay stage, the probe.

`df/droughness` and `df/dmetallic` through the microfacet model. This is the largest piece of new mathematics in the stage.

**THE INSTRUMENT CHANGES HERE, AND THE PLAN ORIGINALLY MISSED THIS.** Task 2's harness compares a perturbed film against an analytic gradient under common random numbers, and that is valid only while the sampled directions do not move under the perturbation. At `metallic = 0, specularWeight = 0` the lobe probability `q` is identically 0 and the direction comes from a cosine-hemisphere draw on `u1,u2` alone — so Task 2's CRN is exact, not approximate.

Neither holds here. `bsdf.glsl:262` samples `sampleGGXVNDF(Vloc, alpha, alpha, uDir)` with `alpha` from roughness, and `q` depends on `F0` and therefore on metallic. **Perturbing either parameter moves the sampled direction**, so a naive CRN finite difference measures the sum of the term you are deriving and a term you deliberately are not — spec §6.3 lists sampled directions as *not differentiated* (detached sampling).

So the reference must be detached too: **hold the sampled directions fixed across `+h` and `-h`, and re-evaluate only `f` and the densities.** That makes the finite difference measure the same quantity the detached adjoint computes. Establish that instrument first — it is the task's real deliverable, and a gate built on naive CRN would report a bias that is by design.

Then, separately, measure the detached-sampling bias itself: compare a *naive* CRN difference against the detached one and report the gap per parameter. The spec says the FD harness is what decides whether detached sampling is acceptable per parameter; this is where that measurement happens, and it is a finding to report, not a gate to pass.

- [ ] **Step 1: Write the failing CRN finite-difference check first**, per parameter, deriving `h` for each — the two parameters have different scales and conditioning, and one `h` will not serve both. Say so with numbers.
- [ ] **Step 2: Confirm it FAILS.**
- [ ] **Step 3: Derive and implement.** Write each adjoint term **from the published formula**, citing the source above it, exactly as Stage 0b-2b's forward oracle did. **Do not differentiate the GLSL by transcription** — an adjoint derived from the same expression it validates cannot fail.
- [ ] **Step 4: Verify** at several roughnesses including near-specular, where the lobe is sharp and conditioning is worst. Report the worst-conditioned case and its error.
- [ ] **Step 5: Demonstrate failure** per parameter — perturb one term of each adjoint, confirm rejection, restore. Paste all of it.
- [ ] **Step 6: Commit.**

---

### Task 4: Emission and light-parameter gradients

**Files:** Modify the adjoint header, the replay stage, the probe.

`dL/dLe` is the simplest adjoint in the stage — the emitted term passes straight through — which makes it the best check on the **plumbing** rather than the mathematics.

- [ ] **Step 1: Write the failing check first.** For a scene whose only parameter is emission, the analytic gradient has a closed form: derive it and state it.
- [ ] **Step 2: Confirm it FAILS. Step 3: Implement. Step 4: Verify.**
- [ ] **Step 5: Demonstrate failure**, and additionally assert the gradient is correct in **magnitude**, not only in direction — a plumbing bug that scales every gradient by a constant passes a direction-only check.
- [ ] **Step 6: Commit.**

---

### Task 5: Texture scatter — the custom bilinear adjoint

**Files:** Modify the adjoint header and replay stage; extend `ParamRegistry` usage in the probe.

A texture read is `sum_i w_i * texel_i` over four texels; the adjoint scatters `dL * w_i` into each. The weights are the same bilinear weights the forward read used, and they must come from the same reconstruction — this is the one place sharing is *required* rather than forbidden.

- [ ] **Step 1: Write the failing check first.** Assert the four scattered weights **sum to the incoming adjoint exactly** (a conservation identity independent of the weights' values), and that a texel outside the footprint receives exactly zero.
- [ ] **Step 2: Confirm it FAILS. Step 3: Implement. Step 4: Verify.**
- [ ] **Step 5: Demonstrate failure** — perturb one weight, confirm the conservation check rejects, restore.
- [ ] **Step 6: Commit.**

---

### Task 6: The four gates — DONE

**Files:** `tests/diff/probe/checks_convergence.{hpp,cpp}` (new, checks 46–49), `tests/diff/diff_gpu_probe.cpp`, `tests/diff/CMakeLists.txt`.

Consolidate Gates 1–4 from the spec: albedo, roughness/metal, emission, light. Each is a CRN finite-difference agreement on a probe-owned scene with a stated, derived tolerance.

**Two things in the wording above turned out to be wrong, and the corrections are the substance of the task.**

*"albedo, roughness/metal, emission, light"* — there is no `light` parameter. Task 4 was titled "Emission and light-parameter gradients" and delivered `DIFF_PARAM_EMISSION` only, because in these scenes an emissive surface **is** the light. The fourth gate is `DIFF_PARAM_EMISSION_TEXTURE`, the spatially-varying emitter, which is the closest thing to a light parameter this stage built and the one with the most machinery to get wrong.

*"confirming the FD error falls as `h^2`"* — true for **one** of the four gates. The order of a central difference is a property of the difference; the order of its **error** is a property of the function. Asserting an `h^2` falloff on emission would have been asserting something false.

| Gate | J's dependence on the parameter | Law asserted | Points needed |
|---|---|---|---|
| 46 albedo | `SUM_{n=1..B} K_n a^n`, exactly | truncation **identically zero** at B=1,2; **exactly** `K_3 h^2` at B=3 | 2 |
| 47 roughness/metallic | smooth, not polynomial | `C h^2 + O(h^4)` — the only asymptotic one | **3** |
| 48 emission | exactly **linear** | truncation identically zero | 2 |
| 49 emission texture | exactly **linear** in a texel | truncation identically zero | 2 |

**How many step sizes a gate needs is set by the polynomial degree of J in the parameter, not by the order of the difference.** A two-point fit reports the gradient's error contaminated by `4*E*h^4`; where J is degree ≤ 3, `E` is exactly zero and two points are provably sufficient. For roughness/metallic `E` is genuinely nonzero and that contamination was measured at up to **5x the gradient's actual error** — a two-point gate there would have rejected a correct adjoint and named the wrong cause.

- [x] **Step 1:** Each gate at two step sizes (three for 47), asserting the law its own analytic form dictates. The pair isolates the error exactly: with `T = (D(2h)-D(h))/3` and `e = D(h)-g`, `T - e = delta` for any `K`. `deltaImplied` is gated at `1e-5 * |g| * caseScale`, one pre-registered number.
- [x] **Step 2:** Roughness/metallic was the one parameter family with no null test (38, 43 and 44 covered the other three). Check 47 now asserts all 255 other arena floats are **exactly** `0.0f`.
- [x] **Step 3: Demonstrated, with perfect specificity.** One line injected into `wf_scatter_replay.comp`'s `diffVertexHook`, scaling one parameter's contribution:

| Perturbation | Rejects | Pre-existing checks |
|---|---|---|
| `BASECOLOR x1.0001` | **46 only** | 37 passes (err 0.1989 ≤ bound 0.5891) |
| `ROUGHNESS x1.002` | **47 only** | 39, 40 pass |
| `EMISSION x1.0001` | **48 only** | 42 passes |
| `EMISSION_TEXTURE x1.00005` | **49 only** | 44, 45 pass |

  The albedo row is the headline: **a 0.01% error in the adjoint that the entire existing Stage 1 gradient suite cannot see.**
- [x] **Step 4: Commit.**

---

## Exit criteria

- [x] `diff_gpu_probe` exits 0 with **54 `OK:` lines**, 0 validation errors, every check existing at the start of this stage still passing with its original expected values, plus the new gradient checks. Report the `OK:` count — a description, never a target.
- [ ] **Replay consumes bit-identical RNG draws to the forward pass at every bounce**, demonstrated to fail on a single inserted draw.
- [x] Gates 1–4 pass at two step sizes each (three for the GGX gate), each asserting the convergence law its own analytic form dictates. **The criterion as written was not satisfiable**: only the albedo's 3-bounce case has an FD error that falls as `h^2`; emission and the emission texture are exactly linear in their parameter, so their truncation is identically zero at every `h`, and asserting a falloff would have been asserting something false. The stronger law is asserted instead.
- [x] Every gradient the scene does not depend on is exactly zero (38, 43, 44, and now 47).
- [ ] Every new check has a **demonstrated failure** recorded with pasted output.
- [ ] The `>1 spp` film-accumulation hazard is resolved explicitly, with the choice stated in code.
- [x] `diff_unit_tests` 28/28, `renderer_test` passes, clean `-j8` build, and `python site/tools/generate_tree.py` exits 0.

## What this stage deliberately does not do

- **No optimisation loop.** Loss kernels, Adam and multi-view batching are Stage 2.
- **No boundary term.** Silhouette gradients need edge sampling or a warped-area reparameterisation; interior gradients come first, and the FD harness is what will later show the boundary term is missing.
- **No device-local arena.** Still the known host-visible sizing issue.
- **No geometry gradients.** Vertex positions are Stage 3.

## Next plan

**Stage 2 — the optimisation loop.** Loss kernels, Adam over the registry, multi-view batching, and Gate 5: recover a known `theta*` on synthetic scenes. The FD harness built here is what makes a failed recovery attributable to the optimiser rather than the gradient.
