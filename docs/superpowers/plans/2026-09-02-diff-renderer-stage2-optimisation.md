# Stage 2 — The optimisation loop

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** loss kernels, Adam over the registry, multi-view batching, and **Gate 5** — recover a known `theta*` on a synthetic scene.

**Gate to pass (spec §9):** Gate 5. Render with known `theta*`, start from `theta_0`, optimise, assert `theta -> theta*`.

---

## What Stage 1 leaves standing

Interior gradients are complete and gated: `dJ/d(albedo)`, `d/d(roughness)`, `d/d(metallic)`, `d/d(emission)`, `d/d(emission texture)`, each against a common-random-number finite difference, each with a demonstrated failure, and each additionally against a **convergence law** at two step sizes (three for the GGX pair) — checks 46–49. 54 `OK:` lines, exit 0.

`ParamRegistry` already allocates a **state block per parameter** (`stateBlock`, `2 * floatCount` — Adam's `m` and `v`). Nothing writes it yet, and checks 38/43/44/47 assert it is **exactly** `0.0f`. Those null tests are the first thing Stage 2 changes, and they should be changed deliberately rather than discovered.

## THE ONE THING THAT MAKES THIS STAGE STRUCTURAL, NOT PLUMBING

Every gradient in Stage 1 is `dJ/dtheta` for **`J` = the sum of every float in the film**. That is not a configuration choice, it is baked into the backward pass's first line. `wf_scatter_replay.comp` says so itself:

> `v.adjoint = v.throughput` SEEDS dL. For a plain sum-of-film objective `dL_0 = 1` and `dL_{b+1} = dL_b * bsdfWeight_b`, which is bit-for-bit the recursion the path's own throughput obeys — so the adjoint at this vertex **IS** the arrival throughput and **needs no path-state field**.

A real loss supplies a **per-pixel** `dL/dpixel`, so `dL_0` stops being 1, the adjoint stops coinciding with the throughput, and **the backward pass acquires a path-state field it does not currently have**. Everything else in this stage is ordinary code. This is the part that changes the shape of the integrator, and it is Task 1 for that reason.

It also comes with the best regression gate available anywhere in this project: **with an all-ones `dL/dpixel` image, every Stage 1 check must still pass, unchanged.** The old behaviour is a special case of the new one, so the entire Stage 1 suite becomes a test of Task 1.

## Inherited hazards — read all of these before Task 1

1. **The `>1 spp` film hazard is now due.** Spec §4.5: the seed invariant covers the path but **not** the film, because several samples of one pixel `atomicAdd` the same three floats and float addition is not associative. Stage 1 dodged it by running 1 spp everywhere and asserting `pixelHits[p] == 1` as a hard failure. An optimisation loop wants more samples per pixel for variance reduction, so **this stage must resolve it explicitly** — 1 spp per dispatch, deterministic per-path reduction, or a documented tolerance. Choose before writing, not after diagnosing.
2. **The arena is host-visible.** Still the known sizing issue; not fixed here, but a multi-view batch makes the arena hotter.
3. **The gradient arena's float `atomicAdd` is non-deterministic** at ~1e-7 relative. Every gate in this stage that compares two runs must go through `probe_normalise.py`, and **that script must be extended for any new arena-derived output** — it silently went stale when Stage 1 Task 6 added checks 46–49, and the staleness was invisible until a baseline was diffed against a second run of the same binary.
4. **Detached sampling (spec §6.3) is still in force.** The optimiser will descend a gradient that omits the sampled directions' own contribution. Check 41 measures that bias at 1–4× the gradient, sign-flipping near-specular. **Gate 5 can fail for that reason alone**, and if it does, that is a finding about the method, not a bug in the optimiser. Budget for distinguishing the two.

## Global Constraints

- **No check weakened, deleted, silenced or renumbered.** New checks take numbers from 50 up.
- **The loss may not reach into the integrator** (spec §4.6). The renderer's contract is exactly: *give me `dL/dpixel`, I give you `dL/dtheta`.*
- Build clean at `-j8`; `diff_unit_tests` passes; `renderer_test` passes; `python site/tools/generate_tree.py` exits 0.
- Every new check gets a **demonstrated failure** with pasted output.

---

### Task 1: The adjoint seed becomes an image — DONE (check 50)

**Files:** `shaders/diff/wf_scatter_replay.comp`, `shaders/includes/diff/traverse.glsl`, `ohao/diff/wavefront/{wavefront_loop,wavefront_buffers}.*`, the probe.

- [ ] **Step 1: Write the failing check first.** Bind a `dL/dpixel` image and assert the scattered gradient equals `SUM_pixels dLdPixel[p] * (dI_p/dtheta)`. With a **non-uniform** image — a single lit pixel is the sharpest case — the answer differs from Stage 1's, so the check fails before the implementation exists and cannot pass by accident.
- [ ] **Step 2: Confirm it FAILS.** Paste it.
- [ ] **Step 3: Implement.** `dL_0 = dLdPixel[pixel]` rather than 1. **Decide and state in code whether the adjoint needs a new path-state field or whether the seed can be folded into the existing throughput carry** — the identity that made it free is exactly what this breaks, so say which way it went and why.
- [ ] **Step 4: THE REGRESSION GATE, and it is the whole reason this task is first.** With an all-ones image, **every Stage 1 check must pass with its original numbers** — normalised diff against a pre-Task-1 baseline must be empty. Anything else means the generalisation changed the special case.
- [ ] **Step 5: Demonstrate failure** — perturb the seed by one pixel's weight, confirm rejection, restore.
- [ ] **Step 6: Commit.**

---

### Task 2: The loss kernel — DONE (check 51)

**Files:** `shaders/diff/loss_l2.comp`, `ohao/diff/loss/`, the probe.

L2 first, because its derivative is a line of algebra and its gradient is checkable without rendering anything at all.

- [ ] **Step 1: Write the failing check first.** `dL/dI_p = 2 (I_p - target_p) / N` for `L = (1/N) SUM (I_p - target_p)^2`. State `N` (pixels? pixels×channels?) — the factor is a real choice and a wrong one is a constant scale error that Gate 5 would absorb into the learning rate and hide.
- [ ] **Step 2: Confirm it FAILS. Step 3: Implement. Step 4: Verify** against a **finite difference on the loss alone** — no renderer involved, so a failure here is unambiguous.
- [ ] **Step 5:** Assert the loss kernel reads nothing from the integrator and the integrator reads nothing from it, by construction and by a source-parsing tie if that is cheap.
- [ ] **Step 6: Demonstrate failure. Step 7: Commit.**

---

### Task 3: Adam over the registry — DONE (check 52)

**Files:** `shaders/diff/optimizer_adam.comp`, `ohao/diff/optimize/`, the probe.

The state blocks exist and are asserted zero today. This is the easy part (spec §5 says so) and its risk is entirely in the checks.

- [ ] **Step 1: Write the failing check first.** Kingma & Ba 2015, **cited by equation number** above the code, bias correction included. Assert the GPU step against a CPU reference over ~50 steps on a **closed-form** objective (a quadratic with a known minimiser) — not against a re-derivation of the same code.
- [ ] **Step 2: Confirm it FAILS. Step 3: Implement. Step 4: Verify.** Report the worst per-step divergence over the trajectory, not just the endpoint: a wrong bias correction is largest in the first few steps and invisible at convergence.
- [ ] **Step 5:** Update checks 38/43/44/47 — the state block stops being zero the moment Adam runs. **Change them deliberately: assert the state block holds what Adam should have written**, rather than deleting the assertion.
- [ ] **Step 6: Demonstrate failure** — drop the bias correction, confirm the early-step assertion rejects while the endpoint still converges. That contrast is the point.
- [ ] **Step 7: Commit.**

---

### Task 4: Multi-view batching, and the `>1 spp` decision — DONE (check 53)

**Files:** the wavefront loop, the probe.

- [ ] **Step 1: Resolve hazard 1 explicitly and in code.** Write down which of the three options was taken and why. **Nothing will test the reasoning**, so it has to be legible.
- [ ] **Step 2: Write the failing check first.** `N` views accumulated into one arena equal the **sum** of `N` single-view gradients. Exactly, up to the arena's own atomic non-determinism — which means through `probe_normalise.py`, with the tolerance stated.
- [ ] **Step 3: Confirm it FAILS. Step 4: Implement. Step 5: Verify.**
- [ ] **Step 6:** Assert the arena is cleared **per iteration, not per view** (spec §4.4) — a per-view clear silently reduces a batch to its last view, and the gradient still looks plausible.
- [ ] **Step 7: Demonstrate failure. Step 8: Commit.**

---

### Task 5: Gate 5 — recovery on synthetic ground truth — SCALAR DONE (check 54); texture remains

**Files:** the probe, `ohao/diff/apply/` if a driver is warranted.

- [ ] **Step 1: State the recovery criterion before running anything.** Which parameter, which `theta*`, which `theta_0`, how many iterations, and **what counts as recovered** — an absolute tolerance on `|theta - theta*|`, pre-registered. A criterion chosen after seeing the trajectory is not a gate.
- [ ] **Step 2:** Recover a **scalar** first (albedo — Stage 1's best-conditioned gradient, exactly linear in its own null test). Then roughness, whose gradient carries the detached-sampling bias.
- [ ] **Step 3:** Then the **emission texture** — many parameters at once, which is where a per-element scatter bug that survived checks 44/45/49 would finally show.
- [ ] **Step 4: If recovery fails, attribute it.** The FD harness is what makes a failure attributable to the optimiser rather than the gradient: if checks 37–49 pass and recovery does not, the gradient is right and the loop is wrong (or the detached bias is the cause — hazard 4). **Say which, with the measurement that decided it.**
- [ ] **Step 5: Demonstrate failure** — start from `theta_0 = theta*` and confirm the loop stays there; perturb the gradient sign and confirm it diverges.
- [ ] **Step 6: Commit.**

---

## Exit criteria

- [ ] `diff_gpu_probe` exits 0; report the `OK:` count — a description, never a target.
- [ ] With an all-ones `dL/dpixel`, every Stage 1 check passes with its original numbers.
- [ ] Adam matches a CPU reference over a full trajectory, not only at convergence.
- [ ] A multi-view batch equals the sum of its single-view gradients.
- [ ] The `>1 spp` film hazard is resolved, with the choice stated in code.
- [ ] **Gate 5: a known `theta*` recovered from `theta_0`** for a scalar, and for a texture, against a pre-registered tolerance.
- [ ] Every new check has a demonstrated failure with pasted output.
- [ ] `diff_unit_tests`, `renderer_test`, clean `-j8`, and `generate_tree.py` all pass.

---

## Where this stands (2026-09-02)

**Gate 5 passes for a scalar.** `theta*` = 0.6 recovered from `theta_0` = 0.3 to **0.599305** — an error of **0.000695** against the pre-registered **0.03**, 43x inside the criterion and 14x tighter than `alpha` itself.

| Task | Check | What it turned on |
|---|---|---|
| 1 adjoint seed | 50 | Seeding `v.adjoint` is WRONG — `diffVertexThroughputAlbedoTerm` reads the throughput directly, so half the gradient stayed unweighted. The partition identity caught it; an all-ones test never could, because at `w = 1` a partially weighted gradient is identical to a correct one. |
| 2 loss kernel | 51 | `N` is the FLOAT count, pinned by a closed form on paper because a per-pixel mean would be 3x off and Gate 5 would absorb it into the learning rate. |
| 3 Adam | 52 | Dropping the bias correction makes the optimiser converge **five times faster** on this objective — an endpoint check would have REWARDED the bug. The non-vacuity bound is now two-sided and rejects it alone. |
| 4 batching | 53 | A per-view clear reduces a batch to its last view, and the result is a real gradient of one view, so only a direct comparison sees it. |
| 5 Gate 5 | 54 | Passes. Demonstrations: sign-flipped gradient runs `theta` to -0.9397 with the loss rising 0.177 -> 4.050; `theta_0 = theta*` reports the loss as EXACTLY 0, confirming `L(theta*) = 0` under CRN rather than assuming it. |

**Still open in Task 5:** Step 3, recovery of the emission TEXTURE — many parameters at once, which is where a per-element scatter bug that survived checks 44/45/49 would finally show. The scalar path is what checks 54 covers.

**Not yet due, and deliberately so:** the plan's Task 3 Step 5 expected checks 38/43/44/47 to need rework because Adam writes the state block they assert is exactly zero. The Adam kernel is generic over caller-owned buffers and the probe passes it its own, so the arena's state blocks are still untouched and those assertions remain true. They come due when a loop drives Adam from the arena directly.

## What this stage deliberately does not do

- **No boundary term.** Silhouette gradients are Stage 3. Interior-only recovery is what Gate 5 tests here, and the FD harness is what will later show the boundary term is missing.
- **No geometry.** Vertex positions are Stage 3.
- **No device-local arena.** Still the known host-visible sizing issue.
- **No perceptual or multi-scale losses.** L2 first; the point of Task 2 is the *interface*, not the catalogue.

## Next plan

**Stage 3 — the boundary term.** Adjacency, silhouette set, edge sampling, vertex gradients, per-iteration BLAS refit.
