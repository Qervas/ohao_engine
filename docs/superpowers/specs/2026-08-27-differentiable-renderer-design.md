# Differentiable Renderer — Design

**Date:** 2026-08-27
**Status:** Design approved, not yet implemented
**Supersedes:** the deleted `ohao/render/diff/` stack (was not a differentiable renderer — see Context)

---

## 1. Context

### What exists today

`ohao/render/diff/` (deleted at HEAD, deletion currently unstaged) was never a differentiable
renderer. Its forward model was `pixel = albedo(u,v) x constant_Lambert_shade` over a ground
plane (`diff_forward.hpp`); `diff_map.hpp:70` `scatterGrad` accumulated `dL/drgb` into the nearest
texel; `diff_optimizer.hpp` ran Adam on those texels. That is a hand-derived gradient for one toy
shading equation with no chain rule through a renderer — no BSDF derivative, no visibility, no
geometry. Deleting it was correct.

`ohao/svbrdf/` (untracked) replaced it with an honest heuristic pipeline: analytic albedo division
plus an 18-candidate grid search over roughness/metal per UV island, with optimization state
round-tripped through PPM files on disk. It is well-labelled and its authors were disciplined about
not overclaiming, but it is not an optimizer, and `estimateLighting` (`schedule.cpp:249`) is a stub
returning the constants its fixtures were baked with.

### What this design is

A real differentiable renderer: gradients of rendered images with respect to registered scene
parameters, computed on the GPU, validated against derivations done on paper.

SVBRDF becomes one client of it rather than the thing itself.

### The engine's distinctive asset

OHAO has a production path tracer **and** a production deferred renderer sharing one scene, one
material SSBO, and one TLAS. Research renderers (Mitsuba) lack the raster half; game engines
(Unreal, Unity) do not expose a differentiable path. This combination is unusual and the
application tracks in Section 10 are chosen to exploit it.

---

## 2. Goals and non-goals

### Goals

- Gradients of rendered images w.r.t. arbitrary registered parameters: textures, BSDF scalars,
  lights, camera/pose, and eventually vertex positions.
- Correctness provable against closed-form derivations and finite differences, not against a
  second implementation.
- Runs on GPU. No CPU reference renderer.
- Parameters live in live engine resources — the optimizer writes what the renderer reads.
- Physically-based and unbiased (the Mitsuba lineage), not soft/approximate (the nvdiffrast
  lineage).

### Non-goals

- Real-time gradient evaluation. Optimization is an offline-ish loop, even if it runs in-engine.
- Competing with feed-forward 3D generative models. See Section 10.4.
- Topology-changing geometry optimization in the first iteration. See Section 9.
- Portability beyond NVIDIA/Vulkan-1.3-with-RT for now.

---

## 3. Decisions and their rationale

| Decision | Choice | Why |
|---|---|---|
| Differentiation scope | Full, including geometry | Chosen target; staged so appearance ships first |
| First milestone | Provably correct gradients | Gradient bugs are silent; optimizing a wrong number is the dominant failure mode |
| Compute | GPU only, no CPU reference | A CPU reference reintroduces the drift disease diagnosed in SVBRDF. Finite differences need only *forward* evaluations, so the GPU renderer is its own oracle |
| AD mechanism | Hand-written GLSL adjoints | PRB makes the backward pass a second forward traversal, not an arbitrary tape — tractable by hand. Keeps the existing toolchain, existing debug tools, and matches the project's stated purpose of understanding the mechanism |
| Pipeline shape | Ray query **wavefront**, not RT pipeline + SBT, and not a megakernel | See S3.1 |
| Discontinuity handling | Edge sampling first, reparameterization later | Edge samples are inspectable — you can draw them and validate the boundary term independently of the interior term |
| Sampling under differentiation | Detached | Standard (Mitsuba PRB). Simpler kernel, well-understood bias. The FD harness measures whether it is acceptable per-parameter |
| Backward memory | Path replay backpropagation | A tape is O(bounces x paths x pixels) and does not fit. PRB is O(1) memory for ~2x compute |
| Host | In-engine, with a headless driver on top | Watching an optimization converge is the primary diagnostic for a silently-wrong gradient |

### 3.1 Why wavefront, not a megakernel (revised 2026-08-28)

This spec originally called for a single ray-query megakernel, on the grounds that
the forward and backward kernels must consume RNG in identical order and therefore
needed the whole path in one function. **That reasoning was wrong**, and the
conclusion with it.

The seed invariant (S4.5) makes a path a pure function of
`(pixel, sampleIndex, iterationSeed)`. Because the sampler is index-addressable,
any stage can *re-derive* the stream from the tuple rather than carrying it. A
wavefront design does not merely tolerate that invariant -- it is the reason
wavefront works here at all, and it is already proven bit-exact between the CPU
reference and the GLSL mirror (`diff_gpu_probe` check 6).

Megakernels have been the wrong default since Laine, Karras & Aila 2013,
*Megakernels Considered Harmful: Wavefront Path Tracing on GPUs*, for three
reasons, each of which differentiation makes worse:

| Cost | Why differentiation amplifies it |
|---|---|
| Registers sized to the worst-case path through every material | adjoint state adds more live values |
| Material/branch divergence across a warp | the backward pass branches the same way |
| Ray incoherence after 2-3 bounces | PRB replays the path, paying the incoherence twice |

The architecture is therefore staged: path state lives in SoA buffers, each bounce
is a separate dispatch, and dead paths are compacted out between bounces. Sorting
by material is deferred until there are enough materials to sort -- the current
BSDF set is Lambert + GGX.

**RenderGraph is deliberately not adopted for this.** The engine has one, written
in-house (`ohao/render/graph/`, 1747 lines), and it does derive barriers
automatically from declared reads and writes. But running its only real consumer
-- the deferred path -- under synchronization validation for the first time on
2026-08-28 reported **29 hazards** (10 WRITE_AFTER_WRITE, 10 READ_AFTER_WRITE,
9 WRITE_AFTER_READ) plus ~300 other validation errors from unrelated pre-existing
defects. Routing the integrator through it would mean a wrong gradient could be a
graph hazard rather than bad math. `ohao/diff/` owns its barriers, with
synchronization validation enabled in its own probe. Fixing the graph is separate
work with its own value.

### Reversibility

The validation harness compares numbers against finite differences and closed-form scenes. It does
not know or care which language produced the gradient. **Slang remains a supported escape hatch** —
if the BSDF set grows large enough that hand-derivation becomes the bottleneck, port behind a
harness that already passes. Slang's autodiff treats global resources (i.e. textures) as
non-differentiable and requires custom derivatives there anyway, so the porting delta is smaller
than it first appears.

---

## 4. Architecture

### 4.1 The interior/boundary split is the top-level structure

```
dI/dtheta  =  INTEGRAL_over_interior  ( df/dtheta ) dx        <- interior term
            + INTEGRAL_over_boundary  ( f * (v . n) ) dl      <- boundary term
```

Two separate kernels, two separate test suites, two gradient contributions summed into the same
arena. When a gradient is wrong you bisect by disabling one.

**For appearance-only parameters the boundary term is exactly zero.** Albedo, roughness, and light
intensity do not move discontinuities. This is not an approximation — it is mathematically absent.
Consequently stages 1–2 never dispatch the boundary kernel, and the entire geometry-gradient
problem is *additive* work that extends stages 1–2 rather than disrupting them.

The split is therefore both the mathematical structure and the staging seam.

### 4.2 The parameter registry is the central abstraction

A `DiffParam` describes what is being differentiated. The integrator does not know what it is
differentiating; it scatters into registered slots.

| To recover | Register |
|---|---|
| SVBRDF maps | albedo / roughness / metal texture images |
| Environment lighting | env map texels |
| Deferred pipeline tuning | SSAO radius/bias, SSR thickness, IBL floor, cascade splits |
| Camera pose | a transform's 6 DOF |
| Object geometry | whatever produces vertex positions (Section 8.4) |
| Neural materials | MLP weights |

Each `DiffParam` carries three handles of identical shape, plus kind metadata:

| | Holds | Written by | Read by |
|---|---|---|---|
| **primal** | current theta | optimizer | renderer (production *and* diff) |
| **gradient** | dL/dtheta | backward kernels, atomically | optimizer |
| **state** | Adam m, v | optimizer | optimizer |

The primitive is "a buffer of floats with a matching gradient buffer." Texture parameters are not a
special case — they carry extra shape metadata for the bilinear scatter. This is what keeps neural
weights, pose, and geometry from each needing bespoke machinery.

### 4.3 Resource ownership: the optimizer writes live engine resources

No export, no round trip, no second copy. This is the strongest available form of the anti-drift
rule: not "share the BSDF code" but "share the actual memory."

**Format constraint.** Production bindless textures default to `VK_FORMAT_R8G8B8A8_SRGB` and
`createTextureImage` makes them sampled-only. These cannot be optimized:

- 8 bits is too coarse for gradient descent. Any Adam step below 1/255 rounds to nothing, so the
  optimizer stalls silently — indistinguishable from convergence.
- sRGB encoding is a nonlinearity between the parameter and the value the shader reads.

A registered parameter's primal must therefore live in a **float32 linear image created with
`SAMPLED | STORAGE` usage**, created by `ohao/diff/` and injected into the bindless array via the
existing `BindlessTextureManager::registerExternalTexture(view, name, type)`. Production samples it
as an ordinary bindless texture; the optimizer writes it as a storage image. One resource.

This also retires the sRGB fight recorded at `ohao/svbrdf/deferred_formation.cpp:38`.

The registry **enforces** this: registering an 8-bit sRGB handle is an error whose remedy is an
explicit `promoteToFloat(handle)` — a visible act, never a silent conversion.

**Gradient arena.** Primals are images (they must be sampled by the renderer). **Gradient and
optimizer-state storage is always buffer-backed, never image-backed** — a texture parameter's
gradient is a buffer of `w*h*c` floats indexed manually, not a storage image.

This is not only an allocation convenience. Measured on 2026-08-27:

| Device | `shaderBufferFloat32AtomicAdd` | `shaderImageFloat32AtomicAdd` |
|---|---|---|
| RTX 5070 Laptop | true | true |
| Intel(R) Graphics (iGPU) | true | **false** |

Image float atomics are materially less available than buffer float atomics. Keeping every
atomic scatter in a buffer removes that dependency entirely.

All gradient and state allocations suballocate from one `VkBuffer`, so per-iteration bookkeeping is
a single `vkCmdFillBuffer` and one barrier rather than N clears over N parameters. With hundreds of
registered maps, per-parameter clears would otherwise dominate the iteration.

### 4.4 Iteration data flow

```
zero arena
  |-> primal render (seed s) ---------> radiance image
  |                                          |
  |                                      loss kernel ---> dL/dpixel
  |                                          |
  |-> interior backward (replays seed s) ----|
  |-> boundary kernel -----------------------|
  |                                          | atomicAdd
  |                                     gradient arena
  '--------------------------> optimizer step (writes primal in place)
```

Exactly two things cross from forward to backward: the loss image and **the seed**.

Multi-view falls out for free — the arena is cleared per *iteration*, not per view, so a minibatch
of views accumulates into the same gradients before one optimizer step.

### 4.5 The seed invariant

**A path is a pure function of `(pixel, sampleIndex, iterationSeed)`.** This is an enforced
property, not a convention: the RNG carries no mutable state across the forward/backward boundary.

It pays for three things at once:

1. **Path replay backpropagation.** Replay must be bit-exact or the backward pass walks a different
   path than the forward pass did, and every gradient is silently wrong.
2. **Common-random-number finite differences.** FD against a Monte Carlo estimator is meaningless
   unless `theta+h` and `theta-h` draw identical samples.
3. **Reproducibility.** A gradient bug that cannot be reproduced cannot be fixed.

The existing Sobol + Owen scramble sampler (`sobol_generator.cpp`, `owen_scramble.cpp`) is already
index-addressable — "sample N of dimension D" as a pure function — so the right sampler is already
in the tree.

### 4.6 Loss lives outside the renderer

The loss is a compute kernel: radiance image + target -> `dL/dpixel`. It is the only component that
knows what is being fitted. L2, L1, masked, multi-scale, perceptual — these proliferate, and none of
them may reach into the integrator.

The renderer's contract is exactly: *give me `dL/dpixel`, I give you `dL/dtheta`.*

**Geometry regularization belongs here**, not in the optimizer. Laplacian smoothness and edge-length
terms need gradients too, and they are losses.

---

## 5. Module layout

```
ohao/diff/
  param/       DiffParam registry - primal handle, grad handle, state handle, kind, shape
  grad/        gradient arena, atomic accumulation, per-iteration zeroing
  optimize/    Adam over parameter blocks (thin - this is the easy part)
  validate/    closed-form scenes, CRN finite differences, single-pixel adjoint dump
  apply/       application tracks (Section 10)

shaders/includes/diff/
  traverse.glsl        THE traversal - loop, RNG order, ray queries; VERTEX_HOOK supplied by includer
  bsdf_lambert.glsl    evalBsdf + evalBsdfGrad side by side
  bsdf_ggx.glsl        evalBsdf + evalBsdfGrad side by side
  texgrad.glsl         bilinear read + atomic bilinear scatter
  dual.glsl            forward-mode dual numbers (validation oracle only)

shaders/diff/
  pt_diff_forward.comp    includes traverse.glsl, VERTEX_HOOK = accumulate radiance
  pt_diff_backward.comp   includes traverse.glsl, VERTEX_HOOK = scatter grad, propagate adjoint
  boundary_silhouette.comp
  boundary_sample.comp
  loss_*.comp
  optimizer_adam.comp
```

---

## 6. The interior kernel

### 6.1 The PRB recursion

Forward at each vertex computes `L = Le + f(theta) * L_i / pdf`. Given an adjoint `dL` arriving at
that vertex, backward does exactly two things:

```
dL/dtheta  +=  dL * (df/dtheta) * L_i / pdf     // scatter into the arena
dL_next     =  dL * f / pdf                     // propagate to the next vertex
```

The only genuinely new mathematics is `df/dtheta` per BSDF.

### 6.2 One traversal source, two instantiations

The backward kernel must walk the identical path, consuming the identical RNG values in the
identical order. Divergence by a single RNG call means the replayed path is a different path and
every gradient is silently wrong — no crash, no NaN.

Under a wavefront design this constraint does not soften -- it sharpens. Path state
crosses a dispatch boundary between bounces, so nothing may rely on registers
surviving. Every stage reconstructs its RNG from `(pixel, sampleIndex, bounce)`
rather than carrying it, which is precisely what makes replay reproducible in the
first place. `drawCount()` remains the tripwire: forward and backward must consume
the same number of draws at every bounce, and that is assertable per stage rather
than only per path.

Therefore **the traversal is one piece of source, included twice**, with a per-vertex hook the
includer defines. Divergence is made structurally impossible rather than prevented by discipline.

This is the surviving half of the rejected "template on scalar type" idea, expressed in the form
GLSL supports.

### 6.3 Differentiable / non-differentiable line

| Differentiable | Not differentiated |
|---|---|
| BSDF evaluation | the ray query itself (visibility is discrete) |
| Texture reads (custom bilinear scatter) | Russian roulette test |
| Emission, throughput products | lobe selection |
| MIS weights (smooth in the pdfs) | **sampled directions — detached sampling** |

**Detached sampling is an approximation, not an identity.** Directions are sampled using detached
parameters; derivatives flow only through the evaluated contribution. If FD and the analytic
gradient disagree on a rough metal, detached sampling is the first suspect. The harness is the
instrument that measures this specific choice.

### 6.4 Texture gradients require float atomics

Bilinear reads scatter into 4 texels weighted by the bilinear weights. The scatter must be
**atomic and order-independent** — never read-modify-write.

This requires `VK_EXT_shader_atomic_float` with **`shaderBufferFloat32AtomicAdd`**, checked
explicitly at device init. Because gradients are buffer-backed (Section 4.3), `shaderImage...` is
*not* required — which matters, since it is the less widely available of the two.

Verified present on the development GPU (RTX 5070 Laptop) on 2026-08-27. Fallback if a target ever
lacks it: fixed-point encoding into uint buffers with integer `atomicAdd`.

### 6.5 Cost

PRB trades memory for compute: **O(1) memory, ~2x traversal cost**, because incident radiance at
each vertex is obtained by re-tracing rather than by storing a tape.

---

## 7. The boundary kernel

Separate dispatch, separate sampling domain — 1D along silhouette edges, not 2D over pixels.

### 7.1 Adjacency vs silhouette

- **`EdgeAdjacency` — built once, topological.** Which edges exist, which triangles share them.
  **This does not change when vertex positions are optimized.** Moving a vertex changes geometry,
  not connectivity. So the CPU-side adjacency build happens once per mesh, ever.
- **`SilhouetteSet` — recomputed per view per iteration, cheap.** A GPU pass over the adjacency
  list marking edges where one adjacent face is front-facing and the other is not, plus open
  boundary edges, then compaction.

This split is what makes per-iteration geometry optimization affordable.

### 7.2 Edge sampling

Each edge sample evaluates radiance on **both sides** of the edge (two rays); the boundary
integrand is their difference, weighted by the edge's velocity under theta. The result scatters
into the gradients of the two vertices defining the edge.

Start with uniform sampling along edges. Hierarchical importance sampling (Li et al. 2018) is a
later optimization, added only once a correct uniform version exists to check it against.

---

## 8. Correctness harness

Five gates, deliberately not sharing implementations with what they test.

### 8.1 Gate 1 — BSDF derivative units

A compute shader evaluates `f` and `df/dtheta` at a few thousand random inputs and compares against
finite differences on `f` alone. **No rays, no scene, milliseconds.** Almost every derivation error
dies here rather than in a full-render gradient that has to be bisected.

### 8.2 Gate 2 — closed-form scenes

Gradients derived on paper:

- Flat quad, constant albedo, directional light, no shadow: `dI/dalbedo = shade`, exactly.
- Emitter seen directly: `dI/demission = 1`.
- Single GGX highlight at known incidence, analytically differentiable in roughness.
- *(stage 3)* A straight edge crossing a pixel — the boundary integral has a closed form under a
  box filter.

Stronger than any cross-implementation check: these test whether the gradient is *right*, not
whether two implementations agree.

### 8.3 Gate 3 — common-random-number finite differences

Full render, seeds pinned, perturb one parameter, central difference.

- **Step size matters.** Too small and float32 cancellation eats the signal; too large and
  truncation error dominates. For central differences in float32, `h ~ 1e-2` to `1e-3` relative to
  parameter scale. A correct gradient looks broken under a badly chosen `h`.
- **Tolerance: relative error < 1e-3** on a few hundred sampled parameters (not all — there are
  millions of texels). Do not set a tighter gate that will later be loosened; float32 accumulation
  over a Monte Carlo estimator does not support 1e-4.

### 8.4 Gate 4 — Mitsuba 3 oracle

Same scene, compare gradient *images*. Catches whole-integrator errors that per-parameter spot
checks miss — a systematically wrong MIS weight derivative is invisible in sampled texels and
obvious in an image diff.

Mitsuba is a numerical oracle here, not a dependency: `pip install mitsuba` plus a comparison
script.

### 8.5 Gate 5 — recovery on synthetic ground truth

Render with known `theta*`, start from `theta_0`, optimize, assert `theta -> theta*`.

This is precisely what `svbrdf/lab.cpp`'s `rebakeViewsWithGroundTruth` closed loop does today —
except here it is an honest *test* rather than a headline result.

### 8.6 Instruments (not gates)

- **Single-pixel adjoint dump.** One pixel, one sample, fixed seed; every vertex's position, BSDF
  value, throughput, adjoint, and gradient contribution written to a readback buffer. This is the
  debugger, and it is what replaces the rejected CPU reference.
- **Gradient visualization.** Render `dL/dtheta` as an image. Wrong gradients usually *look* wrong
  — sign flips at edges, structured noise, hot spots on silhouettes. Nearly free, very high yield.
- **Forward-mode dual numbers** (`dual.glsl`, ~80 lines). Useless for optimization (one pass per
  parameter) but trivially correct, so it is a second independent oracle for hand-derived reverse
  mode.

---

## 9. Staging

| Stage | Builds | Gate to pass |
|---|---|---|
| **0** Scaffolding | Registry, gradient arena, float-atomics device check, seed invariant, forward ray-query wavefront integrator | Forward matches existing PT within MC noise |
| **1** Interior gradients | Hand-derived Lambert + GGX adjoints, texture scatter, PRB replay | Gates 1–4 on albedo / roughness / metal / emission / light |
| **2** Optimization loop | Loss kernels, Adam, multi-view batching, application tracks | Gate 5 — recover known `theta*` on synthetic scenes |
| **3** Boundary term | Adjacency, silhouette set, edge sampling, vertex gradients, per-iteration BLAS refit | Closed-form edge test + FD on positions + Mitsuba silhouette scene |
| **4** Scale | Warped-area reparameterization, edge importance sampling, camera/pose, unify BSDF includes with production PT | No regression on gates 1–5 |

**Stage 3 parameterizes "something that produces vertex positions,"** never vertex positions
directly. Direct positions are the trivial case; the indirection leaves room for Laplacian
preconditioning (Nicolet et al. 2021 — close to mandatory for usable geometry optimization) and for
FlexiCubes-style differentiable iso-surface extraction later, which is the designated escape hatch
if topology change is ever needed. Per-iteration BLAS refit is already budgeted, and
`animated_rt_manager.cpp` already does GPU-skinning-then-BLAS-rebuild, which is structurally the
same problem.

---

## 10. Application tracks

### 10.1 Renderer fitting — the distinctive one

Everyone points differentiable rendering **at reality**. Almost nobody points it **at their own
renderer**.

The deferred pipeline is a pile of approximations with hand-tuned knobs: SSAO radius and bias, SSR
thickness and step count, the equirect IBL floor, ambient fudge factors, CSM cascade splits. Today
they are set by eye. Register them as `DiffParam`s, render ground truth with the path tracer, and
**optimize the approximation to minimize its difference from the path tracer.**

This turns an open item already recorded in `STATUS.md` — *"Deferred metals vs PT — equirect IBL is
a floor, not parity"* — into an objective function.

Prior art, honestly stated: the components are published (neural radiance caching, neural appearance
models, learned shading). The *integration* — gradient-fitting an existing raster pipeline against
one's own offline renderer inside a single engine — is rare, and the reason is that almost nobody
has both halves under one roof.

Lands at stage 2. Requires no new architecture: it is the parameter registry with different
parameters registered.

### 10.2 Sensitivity maps — the renderer's debugger

`dpixel/dtheta` is a sensitivity map. Which light dominates this pixel? Which approximation costs
the most error, and where? The previous cycle used temporal variance maps to chase a flicker;
gradient maps are a strictly sharper instrument for that class of question.

A deliverable, not just an instrument. Cheap once gradients exist.

### 10.3 SVBRDF, rewritten as a client

Ingest a bundle, register three texture params, pick a loss, run the optimizer. `schedule.cpp`'s 943
lines of fixture-tuned heuristics get deleted rather than maintained, and `estimateLighting` becomes
real — environment texels registered as parameters — rather than a stub returning baked constants.

Lands at stage 2.

### 10.4 De-lighting generated assets — future

3D generative models output meshes with lighting baked into albedo — shadows painted into base
colour, nothing relightable. That failure is precisely what a physically-based differentiable
renderer is good at.

This is stage 2 machinery pointed at a different input (synthetic renders of a generated mesh
instead of studio photos), so it needs no architectural change. Deferred to after the tracks above.

---

## 11. Risks and open questions

| Risk | Mitigation |
|---|---|
| `VK_EXT_shader_atomic_float` unavailable | **Retired 2026-08-27** — `shaderBufferFloat32AtomicAdd` verified true on the dev GPU. Buffer-backed gradients avoid the scarcer image-atomic path. Still checked at device init; fixed-point-in-uint fallback documented |
| Detached sampling bias unacceptable for some parameter | FD harness measures it per-parameter; escalate to differentiated sampling only where measured necessary |
| Forward/backward RNG divergence | Shared traversal source with `VERTEX_HOOK`; single-pixel dump to verify |
| Hand-derivation error rate grows with BSDF count | Gate 1 unit tests per BSDF; Slang port remains available behind a passing harness |
| Vertex gradients ill-conditioned (self-intersection, spikes) | Laplacian preconditioning + regularization losses, both planned into stage 3 |
| Local minima / poor initialization | Inherent to the method. Requires sensible initialization and regularization; document rather than pretend otherwise |
| Albedo/illumination ambiguity | Fundamental, not an implementation defect. More light directions, not better optimization |

**Open:** whether the production PT's BSDF includes and the diff BSDF includes converge at stage 4,
or remain deliberately separate. Deferred until stage 4 measurement.

---

## 12. Prior art

- Li, Aittala, Durand, Lehtinen 2018 — *Differentiable Monte Carlo Ray Tracing through Edge Sampling*
- Loubet, Holzschuch, Jakob 2019 — *Reparameterizing Discontinuous Integrands*
- Bangaru, Li, Durand 2020 — *Unbiased Warped-Area Sampling*
- Vicini, Speierer, Jakob 2021 — *Path Replay Backpropagation*
- Nicolet, Jacobson, Jakob 2021 — *Large Steps in Inverse Rendering of Geometry*
- Shen et al. — DMTet (2021), FlexiCubes (2023)
- Laine et al. 2020 — nvdiffrast (the soft/rasterization lineage, for contrast)
