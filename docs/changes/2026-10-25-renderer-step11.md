# Sub-project 2b, step 11: GPU-driven geometry

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("GPU-driven geometry", "The frame after 2b", "Profiling first" (the bench scenes), "Build order" step 11, as built; test plan test 6)
**Status:** Built and tested. Every budget the step gates measured under it, but every `render_bench` run was flagged noisy (outside load, a throttled core), so the verdict is open (section 5).
- The G-buffer and CSM draw only what the GPU lists. Culling is frustum plus two-pass Hi-Z occlusion; the draws are `vkCmdDrawIndexedIndirectCount`. The CPU records the same commands whatever the instance count.
- Lists are compacted by prefix sum, in draw order every run.
- Face culling for one-sided materials, glTF's `doubleSided: false`.
- `bench10k_1080p` and `bench100k_1080p` join `render_bench`.
- Test 6 (`cull_parity_gpu_test`) 21/21; `engine_tests` 80/80; `tools/check.py` 36/36. Render goldens 14/14 after re-baselining the four helmet scenes (face culling: max_abs 1 on 11–15 pixels), in a commit of their own.
- No budget or tolerance was changed.

## 1. Summary

- **Draw table on the GPU.** `RenderScene` keeps its draw list (one record an instance's submesh) as `DrawRecord`s: indices, slot, material slot, sidedness, the mesh's box. It uploads them when the list changes.
- **GPU culling.** `GpuCulling` (`cull/instances.slang`, `cull/hiz.slang`) turns the draw table into indirect lists in three phases:
  - early: last frame's visible draws in the frustum;
  - late: every draw against the frustum and a max-depth pyramid of the early pass's depth;
  - shadow: a CSM cascade's frustum sides.
- **The G-buffer** clears and draws the early lists, builds the pyramid, culls, then loads and draws the late lists. It is timed in five parts: `cull.instances`, `gbuffer.early`, `hiz.build`, `cull.late`, `gbuffer.late`.
- **Face culling.** A one-sided material's draws use a back-face-culled pipeline.

## 2. What was built

- **`shaders/cull/instances.slang`**:
  - `classify`: per draw, the box's 8 corners in clip space; culled when all are outside one plane (a cascade tests only its sides, since CSM clamps depth). In the late phase, the occlusion test and the next frame's visibility. Then a Hillis-Steele scan of the workgroup's 256 memberships in shared memory.
  - `scanBlocks`: one workgroup turns block totals into offsets and writes the counts.
  - `scatter`: writes each member's `VkDrawIndexedIndirectCommand`, with firstInstance = the draw's index.
  - Each phase writes two lists: one-sided and double-sided.
- **Occlusion test.** The corners' NDC rectangle, a pixel wider on each side, is read at the level where it spans at most 2x2 texels. The draw is occluded when its nearest corner's depth is beyond those texels' maximum (by 1e-6). A corner in front of the near plane makes the draw visible.
- **`shaders/cull/hiz.slang`**: `fromDepth` (the depth halved, maximum of 2x2, reads clamped to the edge) and `downsample`. Level 0 is padded to powers of two (section 3).
- **`GpuCulling`** (`ohao/render/deferred/gpu_culling.*`):
  - per-draw buffers (visibility, local offsets, block sums and offsets, counts, 12 command lists) that grow with the draw table;
  - one descriptor set per frame slot, and the pyramid's per-level sets;
  - `beginFrame` (called before the render graph) binds the tables and clears the visibility history after a temporal reset or a new draw-table version;
  - `Settings{frustum, occlusion}`; `setCapture`/`capture()` copies the lists out for tests.
- **`GBufferPass`**:
  - two compatible render passes (early clears; late loads; identical subpass dependencies);
  - `m_cullBackPipeline` (back faces culled, front face counter-clockwise) and `m_pipeline` (double-sided);
  - `gbuffer.vert` reads `draws[gl_InstanceIndex]` for the slot and material slot; the material-slot push constant is gone.
  - The render graph's G-buffer pass is `selfTimed` (`RenderGraph::addPass`).
- **`CSMPass`**: culls each cascade, then draws its list; `shadow_csm.vert` reads the draw table.
- **`RenderScene`**: `drawTable()`, `drawCount()`, `drawsVersion()`; `Draw::oneSided`. **Materials**: `MaterialDesc::doubleSided`, `MaterialTable::doubleSided`, `Model::materialDoubleSided` from Assimp's `TWOSIDED`.
- **Device features**: `drawIndirectCount`, `drawIndirectFirstInstance`.
- **Pass registry**: the five parts replace `gbuffer`. `render_budgets.json` `currentStep` 11.
- **Bench scenes** (`bench_world.hpp`, `buildRenderBenchWorld`): 20 meshes (sphere, cube, platform, cylinder, cone at 4 sizes) on a 400 m grid, a ground, 7 point lights (4 shadowed) and a sun; post on. `render_bench` writes them to `build/render_bench/` before its run.
- **Deleted**: the CPU frustum loop and `ohao/render/culling.hpp` (`Frustum`, `ohao::AABB`; nothing else used them).

## 3. Bugs found and fixed

- **The first pyramid culled visible walls** (Cornell's right wall, the helmet room's): every deferred golden broke. Its levels halved rounding up (68 rows from 135) where Vulkan's mip levels round down (67). The build and the lookup read past the levels, and an out-of-bounds storage read is undefined: the top levels held depths nearer than the scene. Found by reading the 1x1 level alone, which hid every draw. Level 0 is now padded to powers of two, where both roundings agree.
- **Face culling first culled front faces**: the helmet differed on 18% of pixels. The front face is counter-clockwise as rasterized; checked against the helmet goldens and the test's closed boxes (max_abs 0).
- **The two render passes were incompatible** (`VUID-VkRenderPassBeginInfo-renderPass-00904`, 20 messages a run): they share a framebuffer and pipelines, and compatibility includes the subpass dependencies. Both now carry the union of the two.

## 4. Spec corrections

Recorded in the spec's step 11 as-built note and its frame diagram:
- Culling is per draw (an instance's submesh), through a GPU draw table, not per instance.
- The scan is shared-memory Hillis-Steele and a one-workgroup block scan; no subgroup operations.
- No `hiz.final`: the early pass draws last frame's visible set with a frustum test only, so no pyramid is carried between frames.
- The pyramid's level 0 is padded to powers of two.
- Face culling needed material sidedness, which the tree did not have.
- CSM cascades cull their sides only.
- The bench scenes have 8 lights until step 13: 7 points and the sun.

## 5. Results

- **Test 6** (`cull_parity_gpu_test`, 21/21):
  - Culling on and off render identically (max_abs 0, PNG byte for byte for the scene files):
    - the built occlusion scene over 1, 2 and 4 frames: 10 of 12 draws listed in frame 1, 10 drawn early in frame 2, 5 from frame 3;
    - Cornell over 10 frames;
    - the helmet room over 30 frames with TAA, SSAO and bloom.
  - Two fresh renderers give byte-identical counts, visibility and commands.
  - The 5 boxes behind the wall and the 2 off screen are in no list; each of the 3 boxes in view is listed exactly once.
  - One-sided boxes render as double-sided ones (max_abs 0).
- **Suites.** `engine_tests` 80/80 (`sp2b_t01` checks the five new passes are timed in order); `deferred_determinism_gpu_test` 65/65; `check_validation.py` 6/6 clean; `check_live.py` 26/26; `e2e_mcp.py` 20/20; `tools/check.py` 36/36.
- **Goldens** 14/14 after re-baselining. Every scene but the four with the helmet is unchanged (max_abs 0).
  - `damaged_helmet_deferred` and `cli_damaged_helmet_deferred`: max_abs 1 on 12 pixels.
  - `cli_helmet_game_deferred`: max_abs 1 on 11 pixels.
  - `cli_helmet_game_reference`: max_abs 1 on 15 pixels.
  - All of them are single pixels on the helmet's thin tubes, where a back face used to win a depth tie at the silhouette (looked at first, identical run to run).
- **`render_bench --check`** (RTX 5070 Laptop GPU). Three full runs, all noisy: the CPU calibration over its reference, other processes up to 4 cores.

  | budget | limit | last full run | scene alone |
  |---|---|---|---|
  | `cornell_720p` cull+hiz | 0.10 | 0.040 | |
  | `cornell_720p` gbuffer | 0.10 | 0.065 | |
  | `bench100k_1080p` CPU | 0.5 | 0.296 | 0.123 |
  | `bench100k_1080p` CPU / `bench10k_1080p` | 1.2x | 1.175x | 0.95x (0.123 / 0.130) |
  | `cornell_720p` frame | 1.50 | 0.889 | |
  | `helmet_game_720p` frame | 3.0 | 0.953 | |
  | `helmet_game_1080p` frame | 5.0 | 2.029 | |
  | `bench100k_1080p` frame (gated from 13) | 8.0 | 4.029 | |

  In an earlier full run bench100k's CPU was 0.289 ms, 2.37x. The CPU times rise through a run (0.10 ms for the first scene, 0.29 ms for the last) as the core throttles under sustained load. `bench100k_1080p` passes: `cull.instances` 0.056 ms, `gbuffer.early` 1.69, `hiz.build` 0.025, `cull.late` 0.058.
- **`serve_bench`** (noisy): `window_readback` 1.23 ms (2), `window_capture_inline` 7.48 ms (20), `window_capture_png` 6.34 ms (30).

## 6. Known gaps

- `render.settings`' `occlusionCulling` and `geometry` switches arrive in step 15; `GpuCulling::Settings` is set in code only.
- The command and visibility buffers grow by idling the device once, when a draw table larger than any before arrives (a scene load).
- A frame whose camera jumps draws last frame's visible set early regardless; only the frustum test trims it. Correct, but the early pass then draws more than it needs.
- Meshlets (step 12) and clustered lights (step 13) are not here; `bench100k_1080p` keeps 8 lights.

## 7. How to run the new tests

```console
$ build/Release/cull_parity_gpu_test.exe > /dev/null   # gpu
$ build/Release/render_bench.exe --check tests/bench/render_budgets.json > /dev/null   # slow
```
