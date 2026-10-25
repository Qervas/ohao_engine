# Sub-project 2b, step 12: meshlets

**Spec:** `docs/specs/2026-10-24-renderer-upgrade-design.md` ("Meshlets and mesh shaders", "Build order" step 12, as built; test plan test 7)
**Status:** Built and tested. The meshlet-build budget met in a clean run (51.0 ms, budget 60). The last full `render_bench` and `serve_bench` runs were flagged noisy (outside load), so their verdicts are open (section 5).
- Every submesh is split into meshlets at registration (at most 64 vertices, 124 triangles, a bounding sphere and a normal cone each), in three new arenas.
- Draws of 4,096 triangles or more take the meshlet path:
  - a task shader tests each draw's meshlets (cone, frustum, Hi-Z) and mesh shaders draw the survivors;
  - without `VK_EXT_mesh_shader`, a compute pass tests every meshlet and writes one indirect draw per survivor, drawn by a vertex-pulling shader.
- Both paths render the same image as each other and as the instance path (max_abs 0). No golden changed.
- Test 7: `scene_tests` 66/66 (4 new), `cull_parity_gpu_test` 45/45 (24 new). `tools/check.py` 36/36. Goldens 14/14.
- No budget or tolerance was changed.

## 1. Summary

- **Builder** (`ohao/render/scene/meshlet_builder.*`, `buildMeshlets`). It is greedy and deterministic, and adds no dependency.
  - It starts at the first unused triangle, then adds the neighbour sharing the most vertices with the meshlet, oldest candidate first.
  - It skips neighbours whose normal strays more than ~53 degrees from the meshlet's mean.
  - Each meshlet gets its vertices' box centre and farthest vertex as a sphere, and its face normals' mean and widest angle as a cone.
- **Arenas** (`MeshRegistry`): meshlets (`MeshletGpu`, 48 bytes: ranges, sphere, cone), meshlet vertices (mesh-local indices), meshlet triangles (three 8-bit local indices packed in a uint).
  - `DrawRecord.info.zw` is a draw's meshlet range.
  - `DrawRecord.aabbMin.w` is the draw's first meshlet entry: the meshlets of the draws before it.
- **Culling.**
  - A G-buffer phase writes four lists: instance one-sided, instance double-sided, meshlet one-sided, meshlet double-sided.
  - A CSM cascade writes one list, with every draw instanced.
  - Every test lives in one Slang module, `shaders/cull/common.slang`.
- **Drawing.** `shaders/cull/meshlets.slang` holds three entry points: `task`, `mesh`, and `pull` (the fallback's vertex shader). The G-buffer draws a phase's instance lists, then its meshlet lists.
- **`geometry` setting.** `GpuCulling::Settings::geometry` is `auto` (the default), `meshlets` or `instances`. `Settings::meshShaders` set to false forces the fallback. Both are set in code; the protocol switch arrives with step 15's `render.settings`.

## 2. What was built

- **`meshlet_builder`**:
  - The vertex-to-triangle adjacency is CSR.
  - Candidates sit in buckets by shared-vertex count (1-3), each a FIFO; stale entries are skipped.
  - Face normals are computed first, over up to 8 threads from 64k triangles. Each value is its own triangle's, so the split changes nothing.
  - Cone cutoff is sin of the widest angle (meshoptimizer's form); 1 (never culls) when normals spread past ~84 degrees.
  - The function appends to a `MeshletSet`, so a mesh's submeshes share one set.
- **`MeshRegistry::registerMesh`** builds a mesh's meshlets submesh by submesh, uploads them with the rest of the mesh in one staging buffer, and records `MeshInfo::submeshMeshlets`. `RenderScene` adds `meshletEntryCount()` and the three arena accessors.
- **`shaders/cull/common.slang`**:
  - `boxBounds` and `occluded`, moved out of `instances.slang`;
  - `meshletView`: the camera in mesh space by Cramer's rule, and whether the cone test is valid (one-sided, positive determinant);
  - `meshletVisible`: the cone, then the frustum and Hi-Z on the sphere's box.
- **`shaders/cull/instances.slang`**:
  - Scans are `uint4`, one lane per list (`CullPush.extra`: first list, list count, geometry mode).
  - A meshlet list's command is `{groups, 1, 1, draw, 0}`.
  - `classifyMeshlets` and `scatterMeshlets` are the fallback: one thread per meshlet entry, with the entry's draw found by binary search. They reuse `scanBlocks`.
  - The fallback's command is `VkDrawIndirectCommand{triangles * 3, 1, draw << 9, meshlet}`.
- **`shaders/cull/meshlets.slang`**:
  - `task`: 32 meshlets per workgroup, compacted in meshlet order by a shared-memory scan.
  - `mesh`: 64 threads, one per vertex. The draw's matrices are computed once per workgroup in shared memory.
  - `pull`: a vertex shader over the fallback's draws.
  - `shadeVertex` repeats `core/gbuffer.vert`'s operations. Slang has no `inverse`, so the normal matrix comes from GLSL.std.450 `MatrixInverse` through `spirv_asm`.
- **`GpuCulling`**:
  - `cull(cmd, phase, firstList, listCount, viewProj, camera)`, `drawMeshTasks` (`vkCmdDrawMeshTasksIndirectCountEXT`), `drawMeshletFallback`, `meshletFlags`.
  - Per-meshlet buffers grow only when the fallback runs.
  - Counts are `kCullCounts` (16): 12 draw lists and 4 fallback lists.
  - The capture now includes the fallback's commands.
- **`GBufferPass`**:
  - Set 1 has 12 bindings: 6 vertex arena, 7-9 meshlet arenas, 10 culling commands, 11 Hi-Z pyramid.
  - Two push ranges: fragment bytes 0-15; vertex, task and mesh bytes 0-63 (`MeshletPush` from 16).
  - Mesh pipelines and pull pipelines, each in a one-sided and a double-sided version.
- **Device**: `VK_EXT_mesh_shader` (`taskShader`, `meshShader`) when supported; Vulkan 1.1 `shaderDrawParameters`.
- **`render_bench`**:
  - `meshlet_build_1m` (a 708x708 grid, 1,002,528 triangles, median of 9 builds) feeds the `meshletBuild` budget. `currentStep` is 12.
  - `--geometry instances|auto|meshlets` and `--fallback` compare the paths.

## 3. Bugs found and fixed

- **Strip-shaped meshlets.** Taking the newest best candidate grew meshlets along strips: a 300x300 grid's meshlets held 0.50 of the triangle limit. Oldest-first grows compact patches: 0.75 of 124 triangles and 0.995 of 64 vertices. A 64-vertex grid patch holds 98 triangles at most.
- **`VUID-VkShaderModuleCreateInfo-pCode-08740`.** The task shader's `SV_DrawIndex` declares the DrawParameters capability, which needs `shaderDrawParameters`. That feature is now enabled.
- **Cones that never culled.** Built by shared vertices alone, only 56 of the helmet's 248 meshlets had a cone, and in the helmet room the GPU culled none of them. With the normal limit the helmet builds 879 meshlets, all with cones, and 209 are culled in its scene. Random views cone-cull 30% of its meshlet tests, against 0.9% before.
- **The meshlet path was slower for small draws.** At the spec's 256-triangle threshold, `bench100k_1080p`'s 20,000 spheres (256 triangles each, about 3 meshlets) took the meshlet path. Its `gbuffer.early` rose from 1.70 ms to 2.41 ms. At 4,096 it is 1.68 ms (instance path forced: 1.71).
- **Build time.** The normal-coherent builder measured 57.4 ms in a clean run, against 45 ms before; the face-normal pass alone took about 11 ms on one thread. Threading it brought the build to 51.0 ms.

## 4. Spec corrections

Recorded in the spec's "Meshlets and mesh shaders" section and step 12's as-built note:
- The builder takes the oldest best candidate and keeps meshlets within ~53 degrees of their mean normal.
- The meshlet data takes three arenas, not two.
- A G-buffer phase has four lists. CSM keeps the instance path.
- The fallback has no entry table: an entry's draw comes from a binary search, and its draw is carried in firstVertex.
- The task shader reads its draw through `SV_DrawIndex`.
- The meshlet path's images equal the instance path's, not only each other's.
- The cone test applies only to one-sided draws whose matrix keeps winding. Hi-Z applies only in the late phase.
- `kMeshletMinTriangles` is 4,096, not 256.

## 5. Results

- **Test 7, builder** (`scene_tests`, 4 new tests, 66/66). Each test checks: every triangle once, as wound; the vertex and triangle limits; spheres containing their vertices; cones containing their normals; and 512 viewpoints where the cone test never culls a meshlet with a front-facing triangle.
  - Five primitives, including a 64x32 sphere (50 meshlets).
  - The 300x300 grid: 1,936 meshlets, identical across two builds; a second submesh appended to the same set builds as it does alone.
  - Repeated and degenerate triangles.
  - The DamagedHelmet: 879 meshlets.
- **Test 7, parity** (`cull_parity_gpu_test`, 45/45: test 6's 21 and 24 new). Validation clean: 38 renderers, zero messages.
  - **The occlusion scene**, every draw on meshlets, at 1, 2 and 4 frames:
    - mesh shaders = fallback (max_abs 0);
    - fallback culling on = off;
    - the fallback drew 60, 60 and 30 of 72 meshlets.
  - **A one-sided 128x64 sphere** (geometry auto): the cones drop 38 of its 186 meshlets. Mesh shaders = fallback = the instance path.
  - **Twin fallback runs**: byte-identical per-meshlet commands.
  - **Cornell** (every draw on meshlets) and **the helmet room** (auto): PNGs identical across mesh shaders, fallback and instance path. The helmet drew 670 of its 879 meshlets.
- **Suites.**
  - `engine_tests` 80/80; `deferred_determinism_gpu_test` 65/65; `check_validation.py` 6/6; `check_live.py` 26/26; `e2e_mcp.py` 20/20.
  - `tools/check.py` 36/36. Monograph 99/99 units.
- **Goldens** 14/14, none re-baselined. Deferred scenes max_abs 0; `cli_cornell_box_rt_realtime` max_abs 1 on 1 pixel (the BVH floor).
- **`render_bench --check`** (RTX 5070 Laptop GPU).

  | budget | limit | clean full run, step's GPU code | final code, two runs (both noisy) |
  |---|---|---|---|
  | `meshlet_build_1m` build (CPU) | 60 | 57.351 | 57.226 / 50.996 (51.043 clean, alone) |
  | `cornell_720p` frame | 1.50 | 0.892 | 0.879 / 0.931 |
  | `cornell_720p` cull+hiz | 0.10 | 0.040 | 0.039 / 0.041 |
  | `cornell_720p` gbuffer | 0.10 | 0.065 | 0.064 / 0.067 |
  | `helmet_game_720p` frame | 3.0 | 0.966 | 0.962 / 0.995 |
  | `helmet_game_1080p` frame | 5.0 | 2.086 | 2.072 / 2.122 |
  | `bench100k_1080p` CPU | 0.5 | 0.129 | 0.326 / 0.281 |
  | `bench100k_1080p` CPU / `bench10k_1080p` | 1.2x | 1.028x | 1.117x / 1.178x |
  | `bench100k_1080p` frame (gated from 13) | 8.0 | 5.063 | 4.988 / 4.077 |

  - The clean full run passed 18/18. It used the final GPU code and the builder before its face normals were threaded.
  - The final code's two full runs were noisy: the CPU calibration ended 1.58x and 1.51x its reference, and other processes used 1.65 and 1.27 cores.
  - `bench100k_1080p`'s frame rises when it renders last, after the other scenes' sustained load: in the clean run `gbuffer.early` was 2.70 ms there, 1.68 ms rendered alone.
- **Geometry paths** (`render_bench --only ... --geometry`), `gbuffer.early`:
  - `helmet_game_1080p`: instance path 0.202 ms; meshlets 0.222 ms (before the normal limit), 0.215–0.220 ms (with it); 0.232 ms with 32-thread mesh workgroups (rejected).
  - `bench100k_1080p`: instance path 1.71 ms; auto at 4,096 triangles 1.68 ms; auto at 256 triangles 2.41 ms.
- **`serve_bench`**, noisy both runs: `window_readback` 1.20 / 1.22 ms (2), `window_capture_inline` 8.49 / 8.80 ms (20), `window_capture_png` 8.08 / 8.20 ms (30).

## 6. Known gaps

- On the RTX 5070 Laptop GPU the meshlet path costs more than the instance path for a fully visible mesh: about 0.015 ms on the helmet at 1080p. It pays only where culling inside a mesh removes meshlets, and no bench scene has a large, partly visible or self-occluding mesh to measure that.
- `render.settings`' `geometry` and `occlusionCulling` switches arrive in step 15.
- CSM draws every draw on the instance path.
- Wireframe mode applies to instance draws only.
- Every mesh gets meshlets at registration, including meshes that stay on the instance path: about 51 ms of load time per million triangles.
- The fallback's per-meshlet buffers (4 lists x 16 bytes per meshlet) grow by idling the device once, when a larger table first needs them.

## 7. How to run the new tests

```console
$ build/Release/scene_tests.exe > /dev/null
$ build/Release/cull_parity_gpu_test.exe > /dev/null   # gpu
$ build/Release/render_bench.exe --only meshlet_build_1m > /dev/null   # slow
$ build/Release/render_bench.exe --only helmet_game_1080p --geometry instances > /dev/null   # slow
```
