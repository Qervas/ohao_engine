# Sub-project 25, W1-W1d: the path tracer's hit shared, and its cold compile from 11 minutes to about 10 seconds

**Spec:** `docs/specs/2026-11-05-wavefront-path-tracer-design.md` ("Why", steps W1, W1b, W1c, W1d, ruling WF1)
**Status:** built and tested on branch `caustics-step2`. The wavefront split itself (W2-W5) is not started: W1c and W1d
took the compile time it was proposed for, and ruling WF1 now recommends it after caustics 2b and 2c. Not merged.

## 1. Summary

The path tracer's programs compiled cold in 8 to 11 minutes each, once per executable and again beside a live process;
every shader edit reaching raygen cost that before any image, and the gate's GPU suites spent most of their hours there.
A probe (W1b) showed the cost was the code's size, not the ray tracing pipeline: the same path as a compute kernel with
ray queries compiled in the same 689 s. The size came from duplicated call sites: Slang inlines every call for SPIR-V,
so a function reached from k sites is compiled k times, and OpenPBR's entries called one another from several sites
each (`OpenPBRSurface.sample()` alone was 404,296 instructions). Every OpenPBR helper now has one call site per entry,
a path vertex weighs all its directions at one site, and a view of wo reads OpenPBR's tables once. raygen fell from
1,101,869 SPIR-V instructions to 27,895, and with the stages compiled side by side each program compiles cold in about
ten seconds on a loaded machine, once per shader change: at this size the driver honours our pipeline cache file, so
every other executable and a second process beside a live one load it in 0.1-0.2 s. Same arithmetic throughout (the
energy tables integrate byte for byte), and the same frame cost.

## 2. What was built

- **W1, the hit shared** (`shaders/rt/pt_hit.slang`): what the closest-hit and any-hit shaders did, as functions of a
  `HitContext` (instance, primitive, barycentrics, ray, transforms, launch): `fillHitAt`, `anyHitAccepts`. The hit
  shaders call them; a ray query's committed hit can feed the same code. `tracePixel` is a function of the pixel. The
  descriptor set and push constants carry the compute stage for the wavefront's kernels (`PathTracer::kPushStages`).
- **W1b, the probe**: `tracePixel` as a compute kernel with ray queries, timed beside the ray tracing pipeline, then
  deleted.
- **W1c, one call site each** (`shaders/material/openpbr.slang`): each `OpenPBRSurface` entry builds one `OpenPBRView`
  of wo (the viewed base, the fuzz's and the coat's albedos, the coat's path and darkening) and calls `terms()`,
  `pdfFrom()`, `drawShares()`, `sampleDirectionFrom()` and `sampleWeigh()`, `split()`, `coreOf()`, `albedoFrom()` once;
  a thin wall's back is mirrored before the one call. `OpenPBRBase.sample()` is `sampleDirection()` then one `pdf()`
  and one `eval()`; `eval()` mirrors its inputs before its one `evalLobes()`.
- **The path vertex** (`shaders/rt/path_tracer.slang`, `tracePixel` and `cacheUpdate`): the surface's view and draw
  shares once; the light's, the environment's and the bounce's directions drawn first (each from the Sobol group it
  always drew from); then one loop weighs them through one `terms()`, one `pdfFrom()` and one shadow ray. Nothing draws
  inside the loop, so Russian roulette after it reads the bounce group's fourth coordinate as before.
- **W1d, the tables read once a view**: `OpenPBRBase.viewedFrom()` also works out `vE1Avg`, `vMsD`, `vMsM` (the
  material's constants) and `vEd`, `vEm`, `vSlot` (the dielectric, metal and diffuse-slot albedos at the view's mu); the
  base's functions read them, and its own re-views (transmission, inside a solid) are the view itself. A base no view
  made holds NaN there (`unviewed()`).
- **Stages side by side** (`ohao/render/rt/path_tracer_pipeline.cpp`, `createRtPipelineJoined`): each program is created
  through a deferred operation joined by up to 4 threads; the log says how many (`pipeline in N s (T threads)`).
- Deleted: the probe, and caustics 2b's pair-cone search (`pairConeAngle`, the cones, the node score; CPU op 10; tests
  t11 and t13), which ruling M12's seeded walk replaced.

## 3. Bugs found and fixed

- **W1: a pane's transmission lost.** The any-hit called `IgnoreHit()` before writing the shadow payload's transmission
  (which `IgnoreHit` ends the shader at): the glass golden's mean moved 167.152 to 167.223. Fixed before the commit.
- **W1: push constants to fewer stages than their range.** W1 added the compute stage to the range, not to the two
  pushes: every path-traced frame under validation reported VUID-vkCmdPushConstants-offset-01796 (`check_validation`,
  `check_live`, `scene_view_gpu_test`, `deferred_determinism_gpu_test`). W1 had been checked by goldens alone; the gate
  found it. Both now push `kPushStages`.
- **W1d: the CPU twin asked a base no view made** (`openpbr_kernel.slang` op 6, `msLobe`): it now asks a view.

## 4. Spec corrections

- W1b concluded that only the split cures the compile; W1c cured it in the megakernel. The spec's "Why" says so, and
  ruling WF1's recommendation moved from "before 2b's GPU work" to "after 2b and 2c".

## 5. Results

SPIR-V instructions (`spirv-dis --no-color X.spv | grep -vc '^\s*;'`):

| entry | before | W1c | W1d |
|---|---:|---:|---:|
| raygen | 1,101,869 | 40,620 | 27,895 |
| raygenCaustics | 1,147,350 | 41,732 | 29,007 |
| cacheUpdate | 651,498 | 32,477 | 21,331 |
| causticTrace | 125,850 | 41,709 | 29,332 |
| manifoldConnect | 62,418 | 24,891 | 19,995 |
| anyHit | 5,281 | 5,344 | 4,507 |

OpenPBR's entries alone (a compute kernel calling one entry): `sample()` 404,296 to 24,969, `pdf()` 76,068 to 12,878,
`eval()` 44,925 to 10,070, `albedo()` 17,055 to 5,970. Deferred lighting's SPIR-V 1.7x smaller, the transparent pass's
1.8x.

Cold compiles (`PathTracer: <name> pipeline in N s`, a fresh copy of the CLI, the caustics showcase):

| program | before | W1c, one busy core | W1c, others using four cores | W1d, others using three |
|---|---:|---:|---:|---:|
| default | 620-709 s | 10.3 s | 27.3 s | 9.0 s (4 threads) |
| caustics | about 490 s | | 33.7 s | 10.7 s (4 threads) |
| manifold | 4.4 s | | 7.3 s | 5.6 s (4 threads) |

The threads, A/B (each twice, a fresh copy of the CLI and an empty `OHAO_PIPELINE_CACHE_DIR`, others using four
cores): the default program 13.6-13.8 s on 1 thread, 8.4-8.5 s on 4; caustics 18.2-18.4 s and 10.0 s; manifold (one
raygen) 5.7-5.8 s and 5.1-5.2 s.

**The pipeline cache file now serves every executable.** At the old size NVIDIA ignored our `VkPipelineCache` file and
kept the binary in its own cache, one per executable; at the new size it honours the file: a fresh executable loads
each program in 0.1-0.2 s, and so does a second `ohao.exe` started while another one holds the programs (it compiled
from scratch before, 625 s). A cold compile is now once per shader change.

Frame cost (`render_bench --frames 60`, GPU timestamps, the old program and the new one back to back; both runs noisy by
the CPU's calibration, the GPU's calibration within 8% of its reference):

| scene | `pt.trace` old | new | `pt.cache.update` old | new |
|---|---:|---:|---:|---:|
| helmet_rt_1080p | 108.88 ms | 108.80 ms | 3.64 ms | 3.55 ms |
| play_rt_1080p | 99.31 ms | 96.95 ms | 3.61 ms | 3.11 ms |

Gates: the energy tables (`openpbr_tables --check`) byte for byte after each step; `material_tests` 36/36,
`manifold_tests` 12/12. W1c's targeted gate (30 suites, every `gpu` label one included): 26 passed and the four
validation failures above, which passed after the fix; the whole run took 24 minutes (`engine_tests` alone had taken
92 minutes after a shader change). W1d's targeted gate (63 suites: every `gpu`, `quick` and `docs` one the change
reaches): 63/63 in 24 minutes. Render goldens 42/42, five re-captured with notes after looking: the driver rounds
the restructured code differently. The transparency scene's panes are a coin hashed from each ray's bits, so one ulp
re-rolled its path-traced noise (21.8% of pixels, the frame's mean 50.7954 to 50.7956, every 32x32 block within 0.06
levels); glass path traced 33 pixels by 1-2 levels; MetalRoughSpheres deferred and reference one channel of one pixel
by 1 level each.

## 6. Known gaps

- No cold-compile or frame-cost figure on a quiet machine (Unreal Editor used two cores throughout).
- `helmet_rt_1080p`'s `pt.trace` was 69.0 ms on 2026-11-03 (a noisy run, 100 frames) and is 108.8 ms in both of
  today's programs: something between then and W1 (caustics T4 onward) made the realtime path tracer slower. Not
  bisected yet.
- The coat's own table reads (`OpenPBRCoat.albedo`, `.eval`) stay per call site; the Sobol sampler's refill is inlined
  at each of its 17 draw sites in raygen.

## 7. How to run

```console
$ build/Release/openpbr_tables.exe --check ohao/render/material/openpbr_tables.hpp > /dev/null   # slow
$ build/Release/material_tests.exe > /dev/null   # slow
$ build/Release/manifold_tests.exe > /dev/null   # slow
$ python tests/golden/render_golden.py tests/golden/manifest.json > /dev/null   # gpu # slow
```

A cold compile is timed by running a fresh copy of the CLI (NVIDIA's cache is per executable) on a path-traced scene
and reading `PathTracer: <name> pipeline in N s (T threads)` in its log.
