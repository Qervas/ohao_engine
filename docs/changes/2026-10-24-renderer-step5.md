# Sub-project 2b, step 5: validation-clean, deterministic RT

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("Determinism", "Test plan" tests 12 and 13, "Build order" step 5, as built)
**Status:** Built. Test 12 (RT part, the float-atomics lint) and test 13 pass. `tools/check.py` quick + gpu + docs 34/34. Render goldens 11/11. The `rt_realtime` golden was re-baselined in its own commit. Every path-traced tolerance was tightened to the measured BVH floor, 1 / 0.0005 (from 4 / 0.01 and 10 / 0.05). The path tracer is validation-clean through teardown in all four path-traced scenes. `render_bench` ran clean (not noisy); no budget is gated before step 7. No budget was changed, and no tolerance was widened.

## 1. Summary

- **Validation-clean.** The path tracer now runs under the validation layer with zero messages: offline, realtime, realtime through NRD, and the helmet. The layer had been blind to it, because no automated run used it. Under validation the path tracer aborted at creation (NRD's device-function lookup). Behind that were eight distinct problems: VUID 03004, two missing device features, four NRD descriptor/pool/usage errors, and a barrier with a mismatched stage.
- **NRD now runs only when asked for.** It used to run whatever the user asked. `--denoise=nrd` never reached the path tracer: REBLUR, compose and the cinematic chain ran on every RT frame of a profile with AOVs, under `none` and `oidn` too. NRD's jitter and AOV averaging were unreachable.
- **`rt_realtime` is race-free.** Before, renders in separate processes differed in about 43,000 channels (max_abs 15–18). Now 34 of 34 example renders give one pixel hash. The raygen read the accumulation and the normals at neighbour pixels in the same dispatch that wrote them. The accumulation is now a ping-pong pair, and the spatial denoise is a compute pass after the trace.
- **The path tracer's 1-LSB flips have a cause, and it is not ours.** The driver's BLAS builder lays out a mesh's BVH differently from run to run, with identical input. A ray that hits exactly on an edge two triangles share then takes whichever triangle the traversal reaches first. Every mesh has such edges; Cornell's walls are two-triangle quads. Path-traced output keeps a floor of 1 LSB on a few pixels. The helmet flipped in 5 of 83 renders. Cornell never flipped run to run (78 renders), but the scene `ohao run` loads from the file builds another BVH than the example's code, and the two differ by 1 on 10 pixels. The RT goldens and checks are tightened to that floor.
- **The float-atomics lint** rejects `atomicAdd`/`InterlockedAdd` on floats under `shaders/`. There are none today.
- **`OHAO_VALIDATION=sync`** adds the layer's synchronization validation. It reports zero hazards on the path tracer.

## 2. What was built

### Validation (test 13)

- **Binding 12** (bindless textures) is partially bound and update-after-bind only. The variable-count flag is illegal below the layout's highest binding (VUID 03004), and it bought nothing: every set was allocated at full capacity.
- **Device features:**
  - `VK_KHR_push_descriptor` is enabled when supported. NRI resolves `vkCmdPushDescriptorSet` under its KHR name; without the extension the validation layer hides the function, NRI aborts and the process exits 0x80000003.
  - `synchronization2` is enabled, because NRI records `vkCmdPipelineBarrier2`.
- **NRD:**
  - `queuedFrameNum` is `MAX_FRAMES_IN_FLIGHT` (3), not 1. NRI was resetting a descriptor pool that a frame in flight still used (VUID 00313).
  - The seven path tracer images NRD samples gain `SAMPLED` usage (VUIDs 02662, 01211).
  - The compositor and the cinematic post (bloom extract, two blur sets, composite, DoF) keep one descriptor set per frame in flight, chosen by `setFrameSlot` (VUID 03047). They had one set each, rewritten while an earlier frame read it.
- **The DoF barrier** no longer pairs the pre-DoF write with `TOP_OF_PIPE` on frame 1, or names a transfer read under a compute-only stage afterwards (VUID 02819). It now runs `COMPUTE | TRANSFER → COMPUTE` with `SHADER_WRITE`.
- **The NRD chain** runs only in NRD mode (`denoiseMode == NRD`). `applyRTRenderSettings` injects an NRD override into the tracer's settings, as it already did for Atrous and DLSS-RR. Under `--denoise=nrd` the tracer now feeds Halton jitter and AOV averaging to NRD.
- **`check_validation.py`** adds four path-traced scenes: Cornell offline, realtime, realtime with `--denoise=nrd`, and the helmet offline. `scene_view_gpu_test` runs its RT helmet renders under validation and requires zero messages from all six runs.
- **`OHAO_VALIDATION=sync`** chains `VkValidationFeaturesEXT` with synchronization validation (and `VK_EXT_validation_features`) into instance creation.

### Determinism (test 12)

- **Accumulation ping-pong.** Two RGBA32F images. Binding 1 is this frame's (`writeonly` in every raygen) and binding 3, new, is the previous frame's (`readonly`). The tracer swaps them each frame. This frame's image is transitioned from `UNDEFINED` (fully rewritten); the history image is acquired `GENERAL → GENERAL` after the previous frame's write, or from `UNDEFINED` before its first write. `m_accumBuffer` is the image the latest frame wrote (the OIDN readback and DLSS-RR read it). The pool holds 26 storage images per set.
- **The spatial denoise** (`enableInternalDenoise`, on in the realtime profile) moved out of all three raygens into `shaders/rt/pt_spatial_denoise.comp`, run after the trace. Each 16×16 workgroup loads a 20×20 tile of the accumulation and the normals into shared memory, then computes:
  1. variance over the 3×3 neighbourhood;
  2. if the pixel is noisy, one 5×5 edge-aware pass: a Gaussian table × normal similarity^48 (by squaring) × colour similarity;
  3. a luminance rescale, ACES at exposure 0.5 and gamma 2.2, into the RGBA8 beauty.

  When the pass will run, the raygens skip their own beauty store. The old in-raygen version also ran two wider passes that re-read the centre value and were algebraically identity; they are gone. Class `SpatialDenoise` (`ohao/render/rt/denoise/`) keeps one descriptor set per frame in flight.
- **The offline raygen** reads only its own pixel's history; its dead reprojection branch is deleted. `pt_raygen.rgen` (the default shader set, used by an engine test's init) reads history from binding 3 as well.
- **The float-atomics lint:** `reflect_tests` `sp2b_t12_lint_finds_float_atomics` (a sample: a float target, a float literal, a float local and a `float()` value flagged; uint/int adds and comments not) and `sp2b_t12_no_float_atomics_in_shaders` (every shader under `shaders/`).
- **`deferred_determinism_gpu_test`** adds `t12 cornell_rt_offline` and `t12 cornell_rt_realtime` (16 spp, no denoiser, 960×540). Each is compared across two fresh renderers, a child process (raw pixels, not a hash) and a second capture, with validation on. The comparison holds to the BVH floor: at most 16 bytes, each off by 1. Every run so far measured 0; the realtime race differed in tens of thousands.

## 3. Bugs found and fixed

1. **VUID 03004:** binding 12's variable-count flag below 25 higher bindings.
2. **NRD abort under validation:** `VK_KHR_push_descriptor` was never enabled.
3. **`vkCmdPipelineBarrier2` without `synchronization2`** (NRI).
4. **VUID 00313:** NRD's descriptor pool was reset while a frame in flight used it (`queuedFrameNum` 1).
5. **VUIDs 02662 / 01211:** NRD sampled images created without `SAMPLED`.
6. **VUID 03047:** the NRD compositor's and cinematic post's single descriptor sets.
7. **The NRD chain ignored the mode.** It ran on every RT frame with AOVs, writing a binding-30 image only an NRD readback reads: whole frames of unread work under `none` and `oidn`. `--denoise=nrd` never reached the tracer, so NRD's jitter and AOV averaging never ran. Gating the chain exposed this: the NRD readback copied an image nothing had written (VUID 09600).
8. **VUID 02819:** the DoF barrier's stage/access mismatch.
9. **The `rt_realtime` race:** temporal reprojection read `accumBuffer` at a neighbour pixel, and the in-shader denoise read `accumBuffer` and `normalAOV` neighbours, all inside the dispatch that wrote them.
10. **The same race in `pt_raygen.rgen`** (the default shader set).

## 4. Spec corrections

- **"RT offline's 1-LSB flips are fixed":** corrected in "Determinism" and test 12. They are the driver's run-to-run BVH layout, not an engine defect. Evidence:
  - the mesh and texture bytes hash identically every run;
  - the serialized BLAS differs on every run (8,733–9,893 of its 119,719 qwords, in a per-node pattern);
  - synchronization validation finds no hazard;
  - `precise` on the accumulation and tonemap only moves which pixels flip.

  Vulkan has no deterministic-build flag, so the RT goldens are tightened to the floor (max_abs 1, diff_frac 0.0005), not to exact. The spec also notes that test 11's window-vs-headless comparison of path-traced output holds to that floor.

  A first pass set Cornell's goldens to exact, since 24 fresh processes gave one hash. After the spatial denoise's arithmetic changed, the final golden runs caught the CLI realtime twin one compare pixel off by 1, both times. The CLI render is deterministic (18 of 18 one hash), but its scene, loaded from the file, builds another BVH than the example's, and the two differ by 1 on 10 pixels. An exact gate would hang on which pixels happen to sit at a rounding boundary, so Cornell was held to the floor before anything was committed.
- **Build order step 5, as built:** recorded.

## 5. Results

- **`tools/check.py`:** quick 19/19, gpu 13/13, docs 2/2 (34/34).
  - `reflect_tests` 42/42 (2 new).
  - `deferred_determinism_gpu_test` 55/55 (10 new t12 checks).
  - `scene_view_gpu_test` 7/7: RT validation 0+0+0, helmet byte differences 0/0.
  - `check_validation` 6/6.
  - `engine_tests` 61/61, `check_docs` 65/65.
- **Validation, before → after:**
  - Path tracer, before: the NRD abort at creation (exit 0x80000003, Cornell `rt_offline --denoise=none`, the default build).
  - The rest came out one at a time as each was fixed: 03004, `vkCmdPipelineBarrier2` without `synchronization2`, then 00313.
  - The last probe before the usage and descriptor-set fixes (Cornell `rt_offline`, 4 frames, duplicate-message limit 10000): 02662 ×4, 01211 ×32, 02819 ×4, 03047 ×63. Then, once the chain was gated, 09600 ×1 under `--denoise=nrd`.
  - After: 0 in all six `check_validation` scenes. Synchronization validation reports 0 on the helmet offline and on Cornell realtime.
- **Determinism** (fresh processes, 1920×1080, 16 spp, `--denoise=none`):
  - Cornell `rt_offline`: 24/24 one hash.
  - Cornell `rt_realtime`, example: 34/34 one hash after the fix (before: fresh vs fresh max_abs 15–18, 42,685–43,858 differing channels per run, 5 runs).
  - Cornell `rt_realtime`, CLI (`ohao run`): 18/18 one hash. It differs from the example's render by 1 on 10 full-resolution pixels (one compare pixel, `cli_cornell_box_rt_realtime` max_abs 1), because its scene comes from the file and builds another BVH.
  - Helmet `rt_offline`: 83 renders, 5 flipped; 3 of those were the first run after a shader rebuild. Each flip is ≤ 12 fixed pixels at 1 LSB, which is max_abs 1 on 3–4 pixels at compare resolution.
- **Goldens:** 11/11 at the new tolerances. Every scene is at max_abs 0 except `cli_cornell_box_rt_realtime` (max_abs 1 on 1 compare pixel, above).

  | Scene | Old tolerance | New tolerance |
  |---|---|---|
  | `cornell_box`, `cli_cornell_box` | 4 / 0.01 | 1 / 0.0005 |
  | `cornell_box_rt_realtime` and its CLI twin | 10 / 0.05 | 1 / 0.0005 |
  | `damaged_helmet`, `cli_damaged_helmet` | 4 / 0.01 | 1 / 0.0005 |

  The realtime re-baseline, against the old golden: max_abs 8 on 43,824 of 230,400 compare pixels (19.0%), rmse 0.274. That is scattered 1-LSB noise plus a few brighter pixels on the sphere's silhouette, where the race read half-written neighbours. The offline and deferred goldens did not move.
- **Spatial denoise cost** (Cornell `rt_realtime`, 1920×1080): measured as (the frame with the pass dispatched 11 times − the frame with it once) / 10, `render.stats` medians over 120 frames, three runs each.
  - First version: 0.62 ms (15.29 / 15.39 / 16.09 ms vs 21.46 / 21.70 ms).
  - After the squaring and Gaussian table: 0.25 ms (vs 17.58 / 17.92 / 18.37 ms).

  The realtime frame's `pt.render` median is 15.3–16.1 ms.
- **`render_bench`** (not noisy, verdict pass; external load UnrealEditor 36% of a core). `cornell_720p` frame 4.792 ms GPU (step 1 baseline 4.87), `rt.gi` 3.947, `rt.shadows` 0.454. `helmet_game_720p` 5.294 ms, `helmet_game_1080p` 12.656 ms. Every budget is pending until its step (7 at the earliest).
- **Monograph:** 98/98 units cite-check after updates to 12 units, structure 16/16, the site rebuilt (98 pages).

## 6. Known gaps

- **The BVH floor stays:** a cross-process comparison of path-traced output holds to 1 LSB on a few pixels. Removing it would mean making the driver's build order not matter: either the shading of an exact edge hit is made identical for both triangles, or BLASes are built once and loaded serialized (and even then only for one driver).
- **The spatial denoise has no timer of its own:** it is timed inside `pt.render`. `render_bench` has no path-traced scene; RT budgets arrive with steps 7–8.
- **NRD's image quality was not assessed.** A 4-frame NRD Cornell renders and reads back validation-clean, but its look (a camera DoF focused at the world origin, little history) is not this step's subject.
- **`pt_raygen.rgen` stays.** Only the default shader set uses it, and many monograph units cite it, so it is updated rather than deleted.
- **Synchronization validation is a manual switch,** not part of the gate (it is slow).

## 7. Running the new tests

```console
$ build/Release/reflect_tests.exe > /dev/null
$ build/Release/deferred_determinism_gpu_test.exe > /dev/null   # gpu
$ build/Release/scene_view_gpu_test.exe > /dev/null   # gpu
$ python tests/validation/check_validation.py > /dev/null   # gpu
$ python tests/golden/render_golden.py tests/golden/manifest.json --only cornell_box_rt_realtime > /dev/null   # gpu
```

Synchronization validation, by hand:

```
OHAO_VALIDATION=sync build/Release/cornell_box.exe out.png 16 rt_realtime --denoise=none
```
