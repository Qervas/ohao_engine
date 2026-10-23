# Sub-project 3, build step 8: Camera, Environment, scene view and the converted examples

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 8; "Components: authoring vs runtime", "Instantiate system and scene view", "Programs", "Budgets (gated)", test 20, "Risks", open decision 1)
**Status:** Done, except the budget gate (section 5). Save pretty, validate and the bench100k `restore_empty` ratio are over their limits. They wait on open decision 1.

## 1. Summary

Step 8 makes the camera and the environment data. `Camera` and `Environment` are authoring singletons, so a scene file carries them. A host helper applies them to the renderer. `cornell_box` and `model_viewer` now build their worlds from data and gain `--save-scene`. Their five goldens still pass with unchanged tolerances.

The budget gate was re-run on the full spec scene (10,002 entities, with the camera and environment). Save and validate are still over their 10 ms budgets, as in step 7. The `restore_empty` scaling ratio is also over 12x. Both need Frank's ruling (section 5).

## 2. Components (`ohao/ecs/components.hpp`, `ohao/ecs/scene_types.cpp`)

- `Camera {fovDeg, nearPlane, farPlane, active}` is an authoring component, registered as a singleton. Its pose is the entity's world transform: it looks along -Z with +Y up. Roll is ignored. `fovDeg` is limited to 1 to 179 degrees. The near and far planes have small positive minimums.
- `Environment {hdri, intensity}` is an authoring singleton. `hdri` is an optional project-relative path (null for none). `intensity` is unitless, at least 0, with a UI range of 0 to 10.
- Both are one per world, active or not (section 7).
- `checkAssets` also covers `Environment.hdri`. A missing image is `E_ASSET_NOT_FOUND` at `/entities/i/Environment/hdri/path`.

## 3. Scene view and attach

- `ecs::resolveSceneView(world)` (`ohao/ecs/scene_view.hpp`) returns plain values: a `CameraView` (position, pitch and yaw in degrees, fov, near, far) and an `EnvironmentView` (hdri, intensity). `ohao_ecs` does not depend on the renderer camera.
- The camera pose is the entity's world matrix, computed from `Transform` and `Parent` with the transform system's arithmetic. It is correct before the first update.
- `pitchYawDegrees` takes the view direction (world -Z). It computes in double and rounds once. Looking down -Z gives exactly (0, -90), and -0 becomes +0.
- `host::applySceneView` (`tools/ohao/scene_host.hpp`, header-only) is shared by the two examples and by the step 9 CLI. It sets the position, the projection (keeping the render target's aspect) and the pitch and yaw. For the hdri it sets the resolver's absolute path, and it warns if the path is rejected. It always sets the intensity. Without an active `Camera` or an `Environment`, the renderer keeps its own values.
- `ecs::attachModel` (`ohao/ecs/spawn.hpp`) attaches an in-memory model to an existing entity. `spawnModel` is now `create` plus `attachModel`. `saveSceneFile` takes an optional `SaveReport*`.

## 4. Examples

- `examples/example_scenes.hpp` holds the GPU-free builders `buildCornellBox` and `buildModelViewerScene`. The tests build the same worlds. The camera and environment entities come first. They draw and light nothing, so draw and light order are unchanged.
- The Cornell box is five `QuadPrim` walls, two `SpherePrim` entities with one shared mesh, and twelve point lights.
- The model viewer is the framed room from `SceneFramer`, walls, bed, nightstand (quads and boxes), the model, and four framed lights. Its studio hdri is `assets/test_models/env_studio.hdr`.
- A model inside the project (the working directory) is a `ModelAsset`, loaded through the resolver. One outside it is attached in memory, so its save warns `W_UNSAVEABLE_MESH`.
- `examples::extractSaveScene` (`examples/example_cli.hpp`) takes `--save-scene <path>` or `--save-scene=<path>` out of argv, so it may appear anywhere. The world is written with a fixed id seed (`kCornellBoxIdSeed`, `kModelViewerIdSeed`), and the program exits without rendering.
- Save results: `cornell_box` writes 21 entities, and two runs are md5-identical. `model_viewer` writes 17 entities. The `flip` variant writes the 180-degree rotation.
- Both examples exit with code 2 if any `InstantiateError` exists after the first update.

## 5. Results

Release build, single thread, this machine. Final runs, in ms. bench10k is 10,002 entities, 5.13 MB pretty and 3.77 MB compact.

| Operation (bench10k) | Measured | Budget | Result |
|---|---|---|---|
| `saveScene` pretty | 14.36 | 10 | OVER |
| `scene.validate` | 13.99 to 14.66 | 10 | OVER |
| `loadScene`, into an empty world | 18.73 to 19.27 | 20 | ok |
| `takeSnapshot` | 0.53 to 0.67 | 1 | ok |
| `restoreSnapshot`, unchanged | 0.62 to 0.70 | 1 | ok |
| `restoreSnapshot`, 1% of rows changed | 0.49 to 0.50 | 2 | ok |
| `restoreSnapshot`, into an empty world | 2.40 to 2.47 | 10 | ok |
| `world.apply`, batch of 100 `field.set` | 0.168 to 0.181 (one noisy run at 0.212) | 0.2 | ok |
| `world.query`, 1,000 ids, full components | 1.26 to 1.29 | 2 | ok |

- Step 7 measured save at 13.93 ms and validate at 14.22 ms, on bench10k without the camera and environment.
- Scaling (bench100k against the paired bench10k samples): every ratio is within 12x except `restore_empty`, at x12.4 to x16.5.
- An A/B without the two new entities gives x12.0, with the same absolute bench100k time (27.7 ms, against 26.4 to 28.2 ms with them). The ratio moves with the paired bench10k sample. This is not a step 8 regression. It joins open decision 1.

## 6. Tests

- `scene_tests` 55/55, including 6 new `t20_*` tests (test 20 in the spec).
- `api_tests` 15/15, including the new `t20_commands_keep_one_camera_and_environment`.
- `reflect_tests` 30/30. `ecs_tests` 26/26. `engine_tests` 58/58. `runtime_tests` 21/21. `force_generator_tests` 54/54. `physics_backend_tests` all pass. `renderer_test` ALL PASS. `check_scene_schema.py` 14/14.
- `instantiate_gpu_test` 5/5.
- `scene_view_gpu_test` 7/7 (`tests/renderer/scene_view_gpu_test.cpp`, 960x540, a fresh renderer per render):
  - Cornell box, deferred: the hand-set camera and environment, the scene view, and the saved and loaded scene render bit-exact. Validation messages 0 + 0 + 0.
  - Damaged helmet, RT offline: the three renders differ only by the allowed 1-LSB flips (at most 16 bytes, each by 1).
- Render goldens 5/5 PASS, tolerances unchanged:
  - `cornell_box`: max_abs 0;
  - `damaged_helmet`: max_abs 1 (13 px);
  - `cornell_box_deferred`: max_abs 0;
  - `damaged_helmet_deferred`: max_abs 3 (diff_frac 0.0347);
  - `cornell_box_rt_realtime`: max_abs 2 (diff_frac 0.0146).
- Byte-identical to the pre-conversion binaries in one session (md5 equal): `cornell_box` RT at 16 spp, `cornell_box` deferred, and `damaged_helmet` RT at 16 spp.
- `cornell_box_rt_realtime` and `damaged_helmet` deferred vary from run to run with the same binary. They were compared statistically, as the spec's "Risks" section records.
- `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200`: 0 validation messages, including teardown.

## 7. Bugs fixed and rulings

Bugs fixed:
- `OHAO_VALIDATION` turned validation on for any value, even `0`. Now unset, empty or `0` means off (`ohao/gpu/vulkan/device_setup.cpp`).
- The `scene_bench` self-checks assumed exactly n entities. They now count the world (`tools/bench/scene_bench.cpp`).

Rulings (recorded in the spec):
- One `Camera` per world, active or not, through the registry's singleton rule. The spec said one *active* camera. Allowing several later relaxes a constraint and breaks no file.
- Pitch and yaw are computed in double and rounded once, and -0 becomes +0. Roll is ignored.
- `applySceneView` keeps the render target's aspect. It does not reset path-tracer accumulation; a host that changes the view between frames calls `notifyCameraChanged()`.
- Removing an hdri does not unload a map the renderer already loaded, because the renderer has no unload.
- The camera and environment entities come first in the example builders.
- Models outside the project are attached in memory with `attachModel`.
- New test 20 (scene view) in the test plan.

The spec carries these step 8 notes: "Components: authoring vs runtime" (the Camera and Environment row), "Instantiate system and scene view" (the scene view bullet and `attachModel`), "Programs" (as built), "Budgets (gated)" (the step 8 note), test 20, "Risks" (camera parity, shared meshes, run-to-run noise), and build order step 8.

## 8. Known gaps and deferred

- Save pretty (14.36 ms) and validate (13.99 to 14.66 ms) are over their 10 ms budgets. This needs Frank's ruling (open decision 1).
- The bench100k `restore_empty` ratio (x12.4 to x16.5 against 12x) needs the same ruling (open decision 1).
- Run-to-run noise is pre-existing and not gated (spec "Risks"). RT offline flips one byte by 1 in about one run in seven. The deferred helmet varies on about 1.7% of pixels.
- The untracked scratch file `examples/model_viewer_old.cpp` (the A/B helper) could not be deleted in this session. It is not committed. Frank should delete it.
- Next: step 9 (`ohao` CLI, checked-in scenes, scene goldens, CLI goldens, restore-render test).
