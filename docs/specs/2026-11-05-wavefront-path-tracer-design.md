# The wavefront path tracer: splitting the megakernel

**Status:** proposed 2026-11-05 (Frank: "for now it's megakernel, we need to split it up. a nice structure for it").
**Supersedes:** nothing; the path tracer's estimator (`shaders/rt/path_tracer.slang`) is unchanged, only its shape.

## Why

The path tracer is one ray-tracing pipeline whose raygen, `tracePixel`, runs a whole path a thread: about 700 lines
that inline OpenPBR's layered material, the subsurface walk, glass and its crossings, path guiding, the radiance cache,
NRD's and DLSS's guides and the caustics split, about a million SPIR-V instructions (19 MB). That costs three ways:

- **Compile time.** The driver compiles it cold in 8-11 minutes a program (default, caustics), once per executable, and
  again for a second process beside a live one (AGENTS.md). Every shader edit that reaches raygen costs that before any
  image; the gate's GPU suites spend most of their hours there (engine_tests 92 minutes after one shader change).
- **Spills.** What raygen keeps alive across a trace is spilled for every pixel (AGENTS.md: a second trace site made the
  whole tracer 8x slower). Each feature added to the loop taxes every scene.
- **Divergence.** A warp runs every material's code its pixels need, and a long path (glass, a walk) holds its warp's
  short ones.

Production GPU path tracers split the path into stages, each a small kernel over a queue of paths: Laine, Karras, Aila,
"Megakernels Considered Harmful: Wavefront Path Tracing on GPUs" (HPG 2013); pbrt-v4's GPU renderer (Pharr, Jakob,
Humphreys 2023) is the reference design, its stages and queues below are its, in Vulkan compute with ray queries.

## The structure

A path's state lives in buffers (structure of arrays, one slot a path), and a bounce is a sequence of compute dispatches,
each a short kernel that reads the slots it needs, does one thing, and appends what comes next to a queue:

```
camera      generate camera rays                    -> ray queue
loop (until the queues are empty or the bounce limit):
  trace     ray query: nearest hit (alpha, panes,    -> hit queue (by material class), escaped queue
            culled back faces handled inline)
  escaped   environment and MIS for rays that left    -> film
  emitted   a hit light's or emissive triangle's
            radiance, MIS-weighted                    -> film
  shade.*   one kernel per material class: build      -> shadow queue (light / environment / sun samples),
            OpenPBR, sample lights, draw the bounce      ray queue (the next bounce), walk queue
  walk      the subsurface walk, whole: a loop of     -> shade queue (its exit vertex)
            ray queries in one kernel (up to 1,024 steps)
  shadow    ray queries for every queued sample       -> film (its contribution, if visible)
film        add each path's radiance to the running mean; NRD's and DLSS's guides; AOVs
```

- **Material classes** (`shade.*`): the material table already knows a material's class (layered, glass, subsurface,
  plain base). Each class is its own kernel, compiled alone: RT direct light's `shade` / `shadeLayered` split measured
  72 against 126 registers for the same reason. A hit is queued by its material's class.
- **Ray queries, not a ray-tracing pipeline.** `trace` and `shadow` are compute shaders with `RayQuery`; any-hit logic
  (alpha, panes' transmission on shadow rays, culled back faces on camera rays) runs in the candidate loop, as
  `rt/alpha_test.slang` does for RT GI. No SBT, no raygen.
- **Determinism and the sampler.** A path keeps its pixel's slot, so the film adds without atomics (bit for bit as
  today). Each path carries its Sobol state (the pixel's seed and its bounce's dimension group): a stage draws from the
  group the megakernel draws from at the same point, so the sample sequence is the same, and the goldens move only by
  float reassociation.
- **Queues.** Append counters (`InterlockedAdd` on integers, as the radiance cache already does) and indirect
  dispatches sized by them; one bounce's queues double-buffer the next's. Order within a queue does not change any
  pixel's sum (one path a slot).
- **What becomes a stage.** Caustics: the light pass is already its own program (`causticTrace`), the manifold pass
  too; the split's subtraction (`chainThroughput`, `chainOne`) is path state like `throughput`. Path guiding's proposals
  are written by `shade.*`. The radiance cache's lookup ends a path in `shade.*`; its update trace becomes the same
  wavefront over 1 pixel in 25. NRD's guides are written at the first hit by `shade.*` and `escaped`.

## Steps

- **W1 — the hit, shared.** `fillHit` (today the closest-hit shader) becomes a function of (instance, primitive,
  barycentrics) in a module that a ray query's committed hit feeds, so the megakernel and the wavefront decode hits the
  same way; the megakernel switches to it first (`trace` parity with `TraceRay`, every golden unchanged).
- **W1b — where the compile goes (a probe).** The same `tracePixel` built as a compute kernel with ray queries
  (`traceCompute`; tracePixel generic on its tracer, `IPathTracer`: the pipeline's `TraceRay` or a ray query's
  candidate loop through W1's `anyHitAccepts`), its pipeline created beside the ray-tracing one and timed
  (`OHAO_PT_COMPUTE_PROBE=1`). The ray-tracing pipeline compiles each trace site as a continuation (everything live
  across it saved and restored); a compute kernel has none. If the probe compiles in seconds, the compile is the
  pipeline's, and the stages are for divergence and speed; if in minutes, it is the code's size, and only the split
  cures it.
  **Measured 2026-11-05:** the compute kernel's pipeline 688.8 s cold, the ray-tracing pipeline's 620-709 s: the same.
  The compile is the code's size (one function, the whole path inlined), not the pipeline. Only the split cures it, and
  each stage kernel must stay small: the shade kernels carry OpenPBR, and must not carry each other. (Mesa's RADV
  reports the same of merged ray-tracing megashaders, "shader sizes get genuinely ridiculous", fixed by compiling them
  apart: pixelcluster, "Inside Mesa 26.0's RADV RT improvements".) The probe is deleted; the descriptor set and push
  constants keep the compute stage for the wavefront's kernels.
- **W2 — the wavefront for the reference.** camera, trace, escaped, emitted, shade (one class), shadow, film; offline
  profile, no cache, no guiding, no caustics. Held to the megakernel: `check_material` (the white furnace, the closed
  box), `check_analytic`, the path-traced goldens within their floor, Cornell and the helmet room's means.
- **W3 — material classes.** shade.base, shade.layered, shade.glass (crossings, absorption, dispersion), walk.
- **W4 — the rest of the path.** NRD's and DLSS's guides, AOVs, the radiance cache (realtime profile), path guiding,
  the caustics split and its requests, culled faces, panes.
- **W5 — switch and delete.** The wavefront becomes the path tracer; the megakernel and its pipeline go (forward only).
  `render_bench`'s path-traced scenes, compile time per kernel, `check_validation`.

Each step ends with the engine running and every gate green, as the conventions require.

## Tests

- **wf1_parity**: every path-traced golden and `check_material`, `check_analytic` unchanged within their floors with the
  wavefront against the megakernel (same seeds: the sample sequence is the same).
- **wf2_compile**: each kernel's cold pipeline creation timed and logged; the budget is seconds, not minutes.
- **wf3_bench**: `render_bench` `helmet_rt_1080p`, `play_rt_1080p`, `sponza_1080p` path-traced frames against the
  megakernel's medians (pt.trace becomes the stages' sum): no slower; a scene without subsurface or glass faster.
- **wf4_repeat**: two captures in one process bit for bit (no atomics in the film; queues only reorder work).

## Risks

- **Memory traffic.** Path state round-trips through memory every stage (pbrt-v4: about 200 bytes a path); at 1080p
  with one path a pixel, a few hundred MB a frame of reads and writes. Measured in W2; state is kept compact (half
  floats where the estimate allows) if it shows.
- **Small queues late in a path.** Deep bounces have few live paths; their dispatches are short. Measured; a persistent
  final kernel for the tail is the known remedy.

## Rulings for Frank

- **WF1 (OPEN) — when.** Recommended: before step 2b's GPU work, which would otherwise pay a cold compile of the
  caustics program per iteration; 2b's CPU work (the walk, its estimate) is done and waits.
- **WF2 (OPEN) — the realtime profile.** Recommended: the same wavefront (one tracer, as frontier F1 ruled); its
  bounce limit keeps queues short.

## References

- Laine, Karras, Aila, "Megakernels Considered Harmful: Wavefront Path Tracing on GPUs", HPG 2013.
- Pharr, Jakob, Humphreys, "Physically Based Rendering: From Theory to Implementation", 4th ed. (2023), chapter 15
  ("Wavefront Rendering on GPUs").
