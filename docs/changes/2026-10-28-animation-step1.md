# Sub-project 10, step 1: skeletons, clips and GPU skinning on the test rig

**Spec:** `docs/specs/2026-10-28-animation-design.md` ("The data model", "The step", "The frame",
"The renderer", "The agent-facing surface", "Performance budgets", "Test plan" tests 22–24, and
"As built (step 1)")
**Plan:** `docs/plans/2026-10-28-animation.md`, Task 1 (steps 1–8 ticked)
**Status:** built; Task 1 of 4. Row 10 of the roadmap stays open; row 9 stays "built, waiting for
Frank".

## 1. Summary

A skinned mesh now animates and renders everywhere the renderer looks. Animator state is integers
advanced by whole simulation steps, saved and hashed. A pose is a function of that state and the
render alpha; it is presentation, never saved. GPU compute skinning gives each skinned instance a
mesh of its own in the vertex arena. The G-buffer (both geometry paths), CSM, GPU culling, RT
shadows and GI (through a refit BLAS) and the path tracer all read the posed vertices, without
changing how they find a mesh. Everything runs on a generated 4-joint rig: our own content, no
Mixamo, nothing from Somnabulate.

No simulation result changed: every combat, physics and scene test passes unchanged, and all 27
existing goldens are exact. The new saved types moved `world.hash` and `schemaHash` once, in their
own commits.

## 2. What was built

- **The test rig** (`tools/anim/make_test_rig.py`, pure Python, ours; `tests/anim/fixtures/`):
  - a column of four boxes on a four-joint chain (`root` → `lower` → `middle` → `upper`), 96
    vertices, known weights;
  - four clips: `wave` (LINEAR, looping, 2 s), `raise` (one shot, 1 s, 90°), `step` (STEP rotation
    and scale) and `spline` (CUBICSPLINE translation);
  - its animation graph: a full-body replace layer (wave, raise, held, a 1-D blend), an additive
    `upper` layer, and a `tick` event at 0.25 s of wave.

  Two runs write identical bytes.
- **`ohao/anim`** (no ECS, no Vulkan):
  - `gltf_anim`, our own reader of skins and animations: GLB or data URI, normalised accessors, morph
    channels dropped with a warning;
  - skeletons, clips and sampling (nlerp, STEP, the glTF Hermite);
  - blend and additive layers with masks, model matrices and palettes;
  - the `.animgraph.json` parser and validator (each error with its JSON pointer), the layer step,
    pose evaluation and a cache.
- **Components** (saved, with docs, units and limits): `SkinnedMesh`, `Animator` (with
  `AnimLayerState`) and `JointAttach`.
- **The ECS side:** `installAnimation(world, assets)` adds two systems.
  - The `animation` simulate system steps each Animator in id order (a driver hook, transitions,
    advance, clip events into a log of 1,024).
  - The `pose` sync system (before `render_extract`) builds `AnimPose`: model matrices, the palette in
    the mesh's bone order, the previous palette, and the posed local box. It also sets each
    `JointAttach` child's `RenderPose`.

  The CLI context and `LiveHost` install it after combat.
- **The model loader** keeps skinned files' vertices as written (bone names exported; no vertex join
  when a mesh has bones).
- **The renderer:**
  - `RenderScene::registerSkinnedInstance` gives a skinned instance its own mesh: a copied vertex
    range, shared indices and triangle slots, copied meshlets, an updatable BLAS.
  - `setSkinPalette` and `recordSkinning` drive the new passes `skin`, `skin.bounds` and
    `rt.blas.refit` (`shaders/skin/skin.slang`, `SkinPass`). They run before `scene.upload`, only for
    poses that changed.
  - `RTAccelerationStructure::recordBlasUpdate` (refit, or a rebuild every 120 refits) replaces the
    unused `createBLASFromPositions`.
  - `gbuffer.vert` and `cull/meshlets.slang` take a skinned draw's previous position from the copy's
    `boneWeights.xyz` (`kDrawSkinned`).
  - `RenderScene::readMeshVertices` reads a mesh back, for tests and tools.
- **The agent surface:**
  - `animation.info {entity?, since?, max?}`: each animator's layers (state, clip, `t`, fade) and the
    clip events after a cursor;
  - `render.stats` `skinning {instances, skinned, vertices, blasRefits, blasRebuilds}`;
  - a serve session, `tests/serve/sessions/animation.jsonl`.
- **Scenes and benches:**
  - `assets/scenes/anim_rig.scene.json` (the rig mid-wave, a floor, a sun);
  - `render_bench anim16_1080p` (16 rigs in the blockout, the world stepped every frame);
  - `physics_bench` `anim16_step_ms` and `pose16_sync_ms` (`animStep` 1).

## 3. Bugs found and fixed

- **Assimp merged differently-weighted vertices.** `JoinIdenticalVertices` compares position,
  normal and UVs, not joints or weights. It left the rig 72 vertices of 96, the merged seam vertices
  carrying both weight sets. The loader now reads, then post-processes without the join when a mesh
  has bones. `t22_assimp_joint_remap` failed before the fix (72 vertices) and passes after. Every
  existing golden is unchanged.
- **A LINEAR key sampled exactly at its time changed bits.** It went through nlerp and normalize.
  A key now returns its value.
- **Motion vectors after `world.step` or a capture are zero (found, kept).** The previous palette is
  the previous sync's, as `prevWorld` is, and both of those sync more than once before the frame they
  draw. The motion test runs a window frame (simulate, one sync, render) instead. This is recorded as
  an AGENTS.md gotcha.
- **Test bugs of mine, fixed:**
  - an off-by-one in the graph-error test's string replace;
  - `world.step`'s parameter is `frames`, not `steps`;
  - the foot vertices are those weighted wholly to the root joint, not those at y = 0.
- **The serve session's hash differed run to run** until its `scene.load` passed a `seed`, as every
  other session does (the id generator's seed is part of the world).

## 4. Spec corrections ("As built (step 1)" in the spec)

- The pose is sampled at `(steps − 1 + alpha)` steps, and the fade weight is `(faded − 1 + alpha) /
  fade`. The "Frame" section said `steps + alpha`.
- `AnimLayerState.fade` and `faded` are `uint32_t`: the codec has no 16-bit field.
- `installAnimation(world, assets)` is called by the hosts, not by `installCombat`: it needs the
  asset resolver.
- The previous positions live in the skinned copy's `boneWeights.xyz`, flagged by `kDrawSkinned`.
  This replaces a separate stream with an `InstanceGpu` offset, since `InstanceGpu` has no free field.
- Draw culling boxes are conservative boxes from the CPU (each bone's bind box through its palette
  matrix). Meshlet spheres are exact, from the GPU. OPEN ruling 15 recommended exact GPU boxes for
  both.
- No rebuild on a clip change: the renderer does not know clips. The rebuild comes every 120
  refits only.
- `readGltfStructure` (levels) still refuses skins and animations: a level is static.

## 5. Results

- **Tests (this machine, RTX 5070 Laptop):**
  - `anim_tests` 18/18;
  - `skinning_gpu_test` 21/21 under validation, 0 messages;
  - `cull_parity_gpu_test` 49/49 (test 8: culling on = off, mesh shaders = fallback = instances, the
    raised arm drawn with its bind box off screen);
  - `deferred_determinism_gpu_test` 80/80 (the rig: deferred and post bit for bit across renderers
    and a child process; path traced 0 differing bytes);
  - `check_validation.py` 10/10 clean (the rig in deferred, `rt_offline`, `rt_realtime` and NRD);
  - `engine_tests` 86/86, `scene_tests` 84/84, `api_tests` 66/66, `combat_tests` 56/56,
    `level_physics_tests` 59/59, `ecs_tests` 37/37, `reflect_tests` 42/42;
  - `check_serve` 236/236, `check_cli` 29/29.
- **CPU skin against GPU skin:** 96/96 positions and normals within 1e-5 relative. After one window
  frame, 92 of 96 vertices moved; the foot's 4 vertices (joint 0 only) did not.
- **BLAS:** 125 stepped frames gave 124 refits and 1 rebuild. A still pose does no skin, bounds or
  refit work.
- **Goldens:**
  - the 27 existing goldens are exact (deferred max_abs 0; path traced within the floor);
  - new: `cli_anim_rig_deferred` and `cli_anim_rig_reference` (max_abs 0 on a second run) and
    `cli_anim_rig_pt` (max_abs 0 on a second run, tolerance 1/0.0005). Each was looked at before it
    was added.
- **`tools/list`:** 36 tools, 24,137 bytes of 24,576 with `animation_info` (OPEN ruling 13 not
  needed for this step, but 439 bytes are left: a Task 2/3 tool will need it).
  `animation.info` on the rig: 226 bytes.
- **Benchmarks** (medians; both runs were noisy, see below):

| Measure | Bench | Median | p95 | Proposed budget (OPEN ruling 16) |
|---|---|---|---|---|
| `skin` (16 rigs, every pose changing) | `render_bench anim16_1080p` | 0.004 ms | 0.006 | — |
| `skin.bounds` | `render_bench anim16_1080p` | 0.007 ms | 0.008 | `skin` + `skin.bounds` ≤ 0.25 ms |
| `rt.blas.refit` | `render_bench anim16_1080p` | 0.136 ms | 0.382 | ≤ 0.60 ms |
| anim16 frame (1080p, TAA, SSAO, bloom) | `render_bench anim16_1080p` | 2.119 ms | 2.437 | — |
| `animation` step, 16 rigs | `physics_bench anim16_step_ms` | 0.0048 ms | 0.0062 | ≤ 0.05 ms |
| `pose` sync, 16 rigs | `physics_bench pose16_sync_ms` | 0.0195 ms | 0.0299 | ≤ 0.10 ms |

  Both runs were noisy and are not a verdict. `render_bench`: GPU calibration ×1.34 of its
  reference, other processes using 240% of a core. `physics_bench`: CPU calibration ×2.96 of its
  reference, 239% of a core.

  The proposals stand at the spec's values, which the measurements meet with room: refit 0.136
  against 0.60, skin and bounds 0.011 against 0.25, step 0.005 against 0.05, pose 0.020 against
  0.10. **For Frank:**
  - keep the spec's numbers (recommended: they leave room for real characters at ~30 joints and
    thousands of vertices);
  - or tighten them to about 3× the measurements (refit 0.40, skin 0.05, step 0.015, pose 0.06)
    once a quiet run confirms them.

  The play-scene budgets (`play_hud_1080p` skin 0.05 and refit 0.10, the 0.20 ms frame delta,
  `anim_step_ms`, `pose_sync_ms`) wait for Task 2's enemies.
- **Look shots** (`C:\Users\djmax\ohao_refs\look_shots\sp10_anim\`):
  - `step1_rig_strip.png`: wave at 0, 0.5, 1.0 and 1.5 s, then the raised arm, deferred (RT shadow
    and GI) and path traced.
  - `step1_motion_vectors.png`: each vertex's previous position to its current, read back from the
    GPU after one window frame (`skinning_gpu_test --dump`). This is a plot of the motion vectors'
    source data, not the G-buffer's velocity image: the renderer has no velocity debug view.
- **`check.py --all`:** 42 of 51 passed on the first run.
  - Three failures were stale expectations, fixed in their own commits: `e2e_mcp` (35 tools to 36),
    `check_docs` (tool, pass and `world.hash` counts in the guides) and the monograph's citations.
    After the fixes, `check_docs` passed 121/121, `e2e_mcp` 53/53 and the monograph structure tests
    16/16.
  - The goldens (30/30) passed in that run.
  - The six benches came out `noisy` (other processes used about 240% of a core). That is no
    verdict, so their budgets stay unconfirmed: neither a pass nor a regression.
  - The `anim16` numbers that run reported: `skin` 0.018 ms, `rt.blas.refit` 0.168 ms,
    `anim16_step_ms` 0.0051 ms, `pose16_sync_ms` 0.0248 ms. All are inside the proposals, but they
    are noisy too.
  - A final `check.py` (quick, gpu, docs) after the fixes passed 43/43.

## 6. Rulings taken (provisional, recorded for Frank)

- Rulings 1–6 as provisional in the spec: CC0 and our own content, in place with no root motion,
  compute skinning, poses from the step count, the SP10 order, and face, cinematics,
  third-person and ragdoll out of scope.
- **OPEN 12:** refit every changed frame, rebuild every 120 refits; no clip-change rebuild.
- **OPEN 13:** not needed.
- **OPEN 15:** meshlet spheres exact from the GPU; draw boxes conservative from the CPU.
- **OPEN 16:** gating at the spec's proposals; the measurements are above.
- Not implemented here, as instructed: OPEN 7 (the death clip) and OPEN 11 (ADS) belong to Tasks 2
  and 3. `Fox.glb` and `BrainStem.glb` stay in `assets/` (the spec's ruling 10, "OPEN 14" in the
  request); Task 2 deletes them.

## 7. Known gaps

- `t23_restart_resets` (needs `Round`, Task 2).
- The spec's `t24_rt_sees_the_pose` comparison against a static mesh baked into the same pose is not
  built. The test asserts that the deferred frame and the path tracer change in the same place.
- The `t24_refit_and_rebuild` "same image either way" check is not built: there is no API to force a
  rebuild.
- `t24_motion_vectors_of_a_limb` checks the previous positions the shaders read, not the velocity
  G-buffer image.
- `JointAttach` is built and posed but used by no scene yet (Task 2's rifle).
- `ohao schema` does not export the `.animgraph.json` schema; `serve_bench` has no `animation_info`
  measure.
- The draw boxes are conservative: a skinned draw may be kept a little longer than an exact box
  would keep it.

## 8. How to run the new tests

```
build/Release/anim_tests.exe
build/Release/skinning_gpu_test.exe            # GPU; --dump FILE writes the motion CSV
build/Release/cull_parity_gpu_test.exe         # GPU; test 8 is the skinned rig
build/Release/deferred_determinism_gpu_test.exe   # GPU; anim_rig_* rows
python tests/validation/check_validation.py   # GPU; anim_rig rows
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_anim_rig_deferred
python tests/serve/check_serve.py              # the animation session
build/Release/render_bench.exe --only anim16_1080p --check tests/bench/render_budgets.json
build/Release/physics_bench.exe --check tests/bench/physics_budgets.json
python tools/anim/make_test_rig.py             # regenerates tests/anim/fixtures/ byte for byte
```
