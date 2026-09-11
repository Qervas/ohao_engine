# Item 6 — The traced radiance

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** the last Stage 3 deviation closed. The boundary term's two radiances
stop being pushed and start being **traced**.

**Gate:** see below — and the roadmap's statement of it needs correcting before
anything is built.

---

## What is already done, and what is not

The jump now **varies along an edge**: each side is an affine field of screen
position, integrated exactly as a moment (check 64, with the old constant form
as the control it must beat). That was the substantive half of the deviation.

What remains is the radiance coming from the **scene** rather than from a push
constant. `boundary_sample.comp` has five storage-buffer bindings and **no
acceleration structure**: it cannot trace at all today.

## THE ROADMAP'S GATE IS WRONG FOR THE FIRST LAYER, and it matters

`2026-09-12-diff-renderer-remaining-road.md` says item 6 wants "a
convergence-order test, not a fixed tolerance", on the reasoning that a traced
radiance is not affine so the two-point exactness becomes a two-point
quadrature with a truncation term.

That is true of a **shaded** radiance and false of an **emissive** one.

If every surface carries a constant emission, the radiance on each side of a
silhouette is **piecewise constant**, so along any chord that does not cross a
second silhouette the jump is exactly constant — and the moment integral stays
*exact*, not approximate. There is no truncation term to measure the order of.
Writing a convergence gate for that layer would be measuring float noise and
calling it a law.

So item 6 splits, and the two halves want different gates:

| | Radiance | Jump along a chord | Gate |
|---|---|---|---|
| **6a** | traced, emissive (constant per surface) | exactly constant | supersampled oracle, fixed tolerance — check 57's, as check 64 reused it |
| **6b** | traced, shaded (varies over a surface) | varies, non-affine | **convergence order**, checks 46–49's pattern |

6a is the layer that removes the push constant. 6b is the layer that makes the
quadrature approximate. Doing them together would mean introducing the
approximation and the tracing at once, with one gate that cannot say which
moved.

## THE HAZARD THAT GOVERNS BOTH

Every geometry gate so far is attributable **because the interior term is
exactly zero**: spec §4.1's first integral is ∫(df/dθ)dx, and a radiance that
does not depend on θ contributes nothing to it. That is what lets a
supersampled image difference stand in for the boundary integral alone.

A traced radiance keeps that property **only while nothing the trace sees
depends on θ**. Emission attached to the moving object is fine — its *value*
does not change when the vertex moves. Interreflection onto the moving object
is not: move the vertex and the light arriving there changes, the interior term
becomes nonzero, and the oracle stops being a measurement of this integral.

- [ ] **Keep a θ-independent traced case as the GATE** — a fixed emitter, no
      interreflection onto the moving object — and treat the general case as
      *measured* rather than gated. Say which is which in the check's output.

---

### Task 1: the kernel can trace at all

**Files:** `shaders/diff/boundary_sample.comp`, `tests/diff/context/probes_boundary.cpp`.

- [ ] **Step 1:** Add a TLAS binding, the scene vertex/index buffers, and a
      per-primitive emission buffer. `GL_EXT_ray_query` as the traversal, as
      everywhere else in this subsystem.
- [ ] **Step 2: The regression gate first, and it is free.** With tracing
      compiled in but disabled by a push flag, every check from 37 to 68 must
      produce its original numbers through `probe_normalise.py`. A new binding
      that perturbs an existing result means the descriptor set or the push
      block is wrong, and that is much easier to find now than after the
      integrand changes.
- [ ] **Step 3:** Demonstrate failure. Commit.

### Task 2: 6a — the traced emissive jump

- [ ] **Step 1: Write the failing check first** (number 69), against the
      supersampled oracle at check 57's tolerance, unchanged. Two emissive
      surfaces, one occluding the other, and the jump across the silhouette
      read by tracing rather than pushed.
- [ ] **Step 2:** The CONTROL is the pushed form with the same two values: it
      must AGREE, because in this scene the pushed constants are the right
      numbers. That is the opposite of check 64's control and it is the correct
      one here — the claim is that tracing FINDS what a human previously typed
      in, so a disagreement means the trace is wrong, not that the layer is
      necessary.
- [ ] **Step 3:** Then a scene where the pushed form CANNOT agree — three
      surfaces at different emissions, so one constant pair is wrong for at
      least one edge — and require the traced form to beat it against the
      oracle. That is what makes the layer necessary rather than merely
      present.

### Task 3: 6b — the shaded jump, and its order

- [ ] **Step 1:** Give the surfaces a varying radiance (a texture, or a
      position-dependent shading term) that is still θ-independent.
- [ ] **Step 2: A convergence-order gate**, checks 46–49's pattern: the error
      against the supersampled oracle at two or three chord subdivisions, with
      the ratio the derivation predicts. State the predicted order **before**
      measuring it.
- [ ] **Step 3:** The non-vacuity that pattern always needs: the truncation
      term must be measurably present, or the gate is confirming a law on data
      that could not violate it.

### Task 4: the regression gate

- [ ] Checks 1–68 unchanged; `--selfcheck` over four or more runs;
      `portability_check.sh`; `renderer_test`; build clean at `-j8`.

---

## What this deliberately does not do

- **No interreflection onto the moving object.** That makes the interior term
  nonzero and takes the oracle with it. It is not a limitation of the boundary
  term; it is the point at which the boundary term must be *summed with* the
  interior one, which check 61 already established the arena for.
- **No hierarchical edge sampling.** Still Stage 4 (spec §7.2).
