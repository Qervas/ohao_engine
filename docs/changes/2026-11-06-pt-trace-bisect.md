# The realtime path tracer's slowdown, bisected on Linux: guiding out of the default raygen, shadow rays after weighing

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3, F1 T6: real time at 1080p); the slowdown was
recorded as a known gap of `docs/changes/2026-11-05-wavefront-w1.md` (section 6).
**Status:** bisected and fixed on branch `caustics-step2`: `helmet_rt_1080p`'s `pt.trace` 35.0 ms on the RTX 4060
laptop, HEAD before 67.9 ms (its frame 37.6 ms, 71.5 before). Not merged.

## 1. Summary

The W1 record left a gap: on Windows (the RTX 5070 laptop, Unreal Editor running) `helmet_rt_1080p`'s `pt.trace` read
69.0 ms on 2026-11-03 and 108.8 ms at W1. Bisected on Linux (RTX 4060 laptop, driver 615.78), that step does not
exist: caustics step 1's end (`f8e32847`) and W1 (`bac99222`) trace in the same 60.5 ms. Two other costs do, and both
are fixed:

- **Path guiding, off, cost 18 ms** (43.7 to 61.2 ms, `15dd2f01`): its code sat in the default raygen behind a flag.
  Guiding is now a compile-time variant of `tracePixel`, as caustics already was: `raygen` and `raygenCaustics` carry
  none of it, and `raygenGuided` and `raygenCausticsGuided`, each in a program of its own, are built the first time a
  frame guides.
- **W1c and W1d cost 7 ms** (60.5 to 67.9 ms): W1c moved a path vertex's shadow ray into the loop that weighs its
  three directions, so the surface and its `OpenPBRView` (each holding an `OpenPBRBase`, 49 values) were live across
  the trace, and W1d made the view hold more. Every direction is now weighed first, each light sample's light kept
  before visibility (every term of it is linear in the visibility), and the two shadow rays are traced after, at one
  site in a loop of their own.

With both, `pt.trace` is 35.0 ms: 48% under HEAD and 20% under the tracer before guiding; `pt.cache.update` 2.56 ms
(3.55). Images are the same up to rounding (section 4).

## 2. How it was measured

Each point is its own git worktree under `build/bisect/` with the Linux port's minimal patches (the GNU link flags and
oneTBB, assimp's own zlib, the DLSS stub's signature, pipeline layouts kept until teardown, `maintenance4`, the
`shaders` target's Ninja edge, `expand` renamed for Slang 2026, the RPC reader's handle cast), the dependency sources
copied from the main build (no download), and SPIR-V from the same slangc 2026.20. `render_bench --only
helmet_rt_1080p --frames 100` (5 warm-up frames, GPU timestamps), each point with its own pipeline cache directory and
one run first to absorb its cold compile (`bac99222`'s raygen, 1.1 million instructions, 781 s on one thread), then the
points alternated round by round so the laptop's clocks drift over all of them alike. `1149599a` (the 2026-11-01
fix that the W1 record's 31.5 ms came from) does not link on Linux without more of the port; its path tracer is
`3f89183a`'s (only `sampling/vmf.slang`, unused there, differs).

| Point | What it is | `pt.trace` medians | `pt.cache.update` | raygen stack |
|---|---|---|---|---|
| `3f89183a` | 2026-11-02, before path guiding | 43.0, 43.7, 43.7, 43.9 ms | 4.0-4.1 ms | |
| `15dd2f01` | path guiding, off by default | 61.0, 61.1, 61.3, 61.3 ms | 4.4 ms | |
| `1bd3bd6d` | caustics T3 (T1's material split in) | 61.1 ms | 4.4 ms | |
| `f8e32847` | caustics step 1's end | 60.6, 60.9, 60.9, 61.0 ms | 4.4 ms | |
| `bac99222` | W1, the hit shared | 60.2-60.9 ms (7 runs) | 4.6 ms | 864 B |
| `e47c2c80` | W1c, every OpenPBR call site single | 66.1, 66.2, 66.2, 66.3 ms | 3.9 ms | 864 B |
| `f990aa17` | W1d, a view reads the tables once | 67.6-68.4 ms (10 runs) | 3.5-3.6 ms | 896 B |
| HEAD `80c16a56` | 2b on by default | 67.8, 67.9, 67.9 ms | 3.5 ms | 896 B |
| shadow rays after the weighing | | 52.7, 52.9, 53.1, 53.2 ms | 2.6-2.7 ms | |
| and guiding out of the default raygen | this change | 34.9, 34.9, 35.0, 35.0 ms | 2.55-2.57 ms | 512 B (cacheUpdate 352, from 480) |

Caustics T1-T5 cost nothing here (`15dd2f01`, `1bd3bd6d` and `f8e32847` agree). The raygen stack
(`vkGetRayTracingShaderGroupStackSizeKHR`, `OHAO_PIPELINE_STATS=1`) is the most any trace site spills, not how often:
W1c's cost did not show in it; this change's did (896 to 512 bytes).

## 3. What was built

- **Shadow rays after the weighing** (`shaders/rt/path_tracer.slang`, `tracePixel` and `cacheUpdate`): the loop over a
  vertex's light, environment and bounce directions evaluates `terms`, `pdfFrom` and the MIS weights for all three as
  before, but a light or environment sample's contribution (and its diffuse and specular parts for NRD) is kept
  without visibility; a loop of two then traces the shadow rays at the vertex's one `visible()` site and adds each
  contribution times what its ray brings back (1, or the panes' transmittance). Nothing draws in either loop: every
  Sobol group and Russian roulette's draw is as it was. A sample whose shadow ray is blocked now pays its `pdfFrom`.
- **Guiding a compile-time variant** (`tracePixel<CAUSTICS, GUIDING>`; `PathTracer::ensureGuidedProgram`,
  `render/rt/path_tracer_pipeline.cpp`): `raygenGuided` (the default program's raygen with guiding, its own pipeline;
  the radiance cache's update stays the default program's) and `raygenCausticsGuided` (with `causticTrace`, as
  caustics' program), made the first time a frame guides (3.0 and 3.2 s cold); a frame that guides binds the one
  beside the program it would trace with. If one cannot be built, guiding stays off.
- SPIR-V instructions: `raygen` 29,431 to 26,317, `raygenCaustics` 27,463, `raygenGuided` 29,571,
  `raygenCausticsGuided` 30,705; `cacheUpdate` 22,692 to 22,721.

## 4. Results

- `render_bench`, `helmet_rt_1080p` (section 2): `pt.trace` 34.9-35.0 ms in alternation with HEAD's tracer at
  68.4 ms; the frame 37.5-37.7 ms (71.4-72.1). `play_rt_1080p` (alternating with W1d, two rounds each): `pt.trace` 28.5, 28.5 ms against
  55.6, 55.7 ms; the frame 30.8, 30.9 ms against 59.4, 59.6 ms.
- Render goldens: 42 of 42. Six path-traced scenes (`cornell_box`, `damaged_helmet`, `cli_cornell_box`,
  `cli_damaged_helmet`, `cli_materials_step2_pt`, `cli_glass_pt`) moved 2 or 3 pixels by 1 level, the same pixels
  run after run; HEAD's shaders reproduce the old goldens exactly, so the restructured code rounds differently there.
  Compiling guiding out moved nothing further. The four images (the `cli_` scenes share theirs) are re-captured, each
  noted in the manifest.
- Guiding, which no test renders on the GPU: Cornell (`raygenGuided`) and `caustics.scene.json`
  (`raygenCausticsGuided`) at 480x270 with `--guiding`, against W1d's tracer. At 1 sample, before anything is
  learned, they agree to rounding (9,572 pixels differ, at most 4.7e-5 in linear radiance, the means equal to 6
  digits); from the second sample guiding learns from those roundings and the noise differs (Cornell at 64 spp:
  means 0.333739 and 0.333638).
- Path guiding's closed forms, new (no GPU test rendered with guiding before): `check_material`'s closed box with
  `--guiding` on `raygenGuided`, and `check_caustics`' `c3_slab_guided` on `raygenCausticsGuided` (the slab's floor
  1.02% from its closed form, as `c3_slab`'s 1.01%); each also fails if its program was never built.
- GPU gate (`tools/check.py --labels gpu`): 26 of 26, no GPU fault in the kernel's log. Render goldens 42 of 42.
- The NVIDIA GPU's firmware failed three times while these were measured (Xid 120 on 2026-11-05, Xid 62 and Xid 119 on
  2026-11-06, each during a GPU test, each needing a reboot); `check_caustics` passed 16 of 16 alone on the same code
  after the second, and `nvidia-persistenced` now keeps the GPU initialised between test processes (the laptop had
  powered it off between every one).

## 5. Known gaps

- The Windows numbers (69 to 109 ms) are unexplained: that machine is another GPU and ran Unreal Editor throughout.
  Nothing between `f8e32847` and `bac99222` costs anything on Linux.
- The realtime trace's budget (`tests/bench/render_budgets.json`, 18 ms, from F1 T6's 16.3 ms on the RTX 5070
  laptop) is not yet gated and was set on the other GPU; on this one the trace is 35.0 ms.

## 6. How to run

```console
$ build/Release/render_bench --only helmet_rt_1080p --frames 100 > /dev/null   # gpu
```
