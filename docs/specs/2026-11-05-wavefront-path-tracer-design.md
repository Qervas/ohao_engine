# The wavefront path tracer: splitting the megakernel

**Status:** proposed 2026-11-05 (Frank: "for now it's megakernel, we need to split it up. a nice structure for it").
**Supersedes:** nothing; the path tracer's estimator (`shaders/rt/path_tracer.slang`) is unchanged, only its shape.

## Why

The path tracer is one ray-tracing pipeline whose raygen, `tracePixel`, runs a whole path a thread: about 700 lines
that inline OpenPBR's layered material, the subsurface walk, glass and its crossings, path guiding, the radiance cache,
NRD's and DLSS's guides and the caustics split, about a million SPIR-V instructions (19 MB). That costs three ways:

- **Compile time.** The driver compiled it cold in 8-11 minutes a program (default, caustics), once per executable, and
  again for a second process beside a live one (AGENTS.md). Every shader edit that reached raygen cost that before any
  image; the gate's GPU suites spent most of their hours there (engine_tests 92 minutes after one shader change).
  **Solved without the split** (W1c and W1d below: every function called from one site, the tables read once a view,
  a program's stages compiled side by side; raygen 39x smaller, each program's cold compile about ten seconds with the
  machine loaded). The split stays for the other two.
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
  The compile is the code's size (one function, the whole path inlined), not the pipeline. (Mesa's RADV reports the same
  of merged ray-tracing megashaders, "shader sizes get genuinely ridiculous": pixelcluster, "Inside Mesa 26.0's RADV RT
  improvements".) The probe is deleted; the descriptor set and push constants keep the compute stage for the
  wavefront's kernels. *Spec correction:* this paragraph concluded that only the split cures the size; W1c cured it in
  the megakernel.
- **W1c — one call site each (measured 2026-11-05).** Where the size came from: Slang inlines every call for SPIR-V
  (`[noinline]` is not honoured there), so a function reached from k call sites is compiled k times, and OpenPBR's
  entries called each other from several sites each. `OpenPBRSurface.sample()` was 404,296 instructions: `sampleFront`
  twice (a thin wall's two sides), in it the base's `sample()` twice and `eval()` twice, in each `eval()` the base's
  `evalLobes()` three times, and `coatDarken` (the base's whole albedo) in every `baseScale`. Each OpenPBR entry now
  builds one `OpenPBRView` of wo (the viewed base, the fuzz's and the coat's albedos, the coat's path and darkening)
  and calls each helper from one site (`terms`, `pdfFrom`, `drawShares`, `sampleDirectionFrom` then `sampleWeigh`; a
  thin wall's back mirrored before the one call): `sample()` 24,969, `pdf()` 12,878 (76,068), `eval()` 10,070
  (44,925), `albedo()` 5,970 (17,055). A path vertex (`tracePixel`, `cacheUpdate`) builds its view once, draws its
  light's, the environment's and the bounce's directions, then weighs the three in one loop at one evaluation, one
  density and one shadow-ray site; nothing draws inside the loop, so every Sobol group and Russian roulette's draw are
  as they were. Instructions: raygen 1,101,869 to 40,620, raygenCaustics 1,147,350 to 41,732, cacheUpdate 651,498 to
  32,477, causticTrace 125,850 to 41,709, manifoldConnect 62,418 to 24,891; deferred lighting's SPIR-V 1.7x smaller,
  the transparent pass's 1.8x. Cold compiles (`ohao_cold1.exe`, a fresh copy of the CLI): the default program 620-709 s
  to 10.3 s beside one busy core, 27.3 s with Unreal Editor and others using four; the caustics program about 490 s to
  33.7 s, the manifold program 7.3 s (both beside the four). Same arithmetic: the energy tables integrate byte for byte
  as before, material_tests 36/36, manifold_tests 12/12, every deferred golden exact (max_abs 0), the path-traced ones
  within their floor except two the driver's new rounding re-rolled (re-captured: the transparency scene's panes are a
  coin hashed from each ray's bits, so one ulp anywhere re-rolls the frame's noise, its mean 50.7954 to 50.7956; glass
  33 pixels by 1-2 levels). The split is no longer needed for compile time; it is for spills and divergence, measured
  against this megakernel.
- **W1d — a view reads the tables once; stages compile side by side (measured 2026-11-05).** After W1c the largest part
  of each kernel was OpenPBR's table reads, repeated per helper: `pbrGridCoord` inlined 542 times in raygen,
  `dielectricAlbedo` 28. `OpenPBRBase.viewedFrom` now also works out the material's constants (the lobe's mean E1 and
  both multiple-scattering Fresnels) and the albedos at the view's mu (dielectric, metal, the diffuse slot); the base's
  functions read them, and its own re-views (transmission, inside a solid) are the view itself. raygen 40,620 to 27,895
  instructions, raygenCaustics 41,732 to 29,007, cacheUpdate 32,477 to 21,331, causticTrace 41,709 to 29,332,
  manifoldConnect 24,891 to 19,995. Each program is created through a deferred operation joined by up to 4 threads, so
  its two raygens (now about the same size) compile side by side. Cold, with other processes using three cores as in
  W1c's second figure: the default program 27.3 s to 9.0 s, the caustics program 33.7 s to 10.7 s, the manifold
  program 7.3 s to 5.6 s; the size alone predicts about 14 s and 17 s for the first two.
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

- **WF1 (OPEN) — when.** Recommended: after caustics 2b and 2c. W1c took the cold compile from minutes to seconds in the
  megakernel, so 2b's GPU work no longer waits on the split; W2-W5 are then measured for speed (spills, divergence)
  against a megakernel that compiles as fast as they would.
- **WF2 (OPEN) — the realtime profile.** Recommended: the same wavefront (one tracer, as frontier F1 ruled); its
  bounce limit keeps queues short.

## References

- Laine, Karras, Aila, "Megakernels Considered Harmful: Wavefront Path Tracing on GPUs", HPG 2013.
- Pharr, Jakob, Humphreys, "Physically Based Rendering: From Theory to Implementation", 4th ed. (2023), chapter 15
  ("Wavefront Rendering on GPUs").
