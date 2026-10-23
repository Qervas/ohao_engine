# Sub-project 3, build step 9: ohao CLI, checked-in scenes, goldens and the restore test

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 9; "Programs"; the CLI section, "As built (step 9)"; the `render.capture` row of the method table; the "Rendering and host (step 9)" error row; "Test plan" tests 14, 15, 17 and 18; "Budgets (gated)", step 9 note)
**Status:** Done. Every suite passes and every bench10k budget passes. The bench100k `restore_empty` ratio is over its line and is reported (section 8). The codec speed rewrite, the first part of this step, has its own record: [2026-10-23-reflect-codec-speed.md](2026-10-23-reflect-codec-speed.md).

## 1. Summary

Step 9 adds the agent-facing command line and the render path behind it. `ohao` (`tools/ohao/main.cpp`) wraps `api::Session`. It creates no Vulkan device unless something renders. `render.capture` starts a renderer on first use. Two checked-in scenes can now be loaded by the CLI. Five `cli_*` goldens check that the CLI renders the same images the examples do. A restore test checks that a restored world renders as it did before a change.

Building this turned up five bugs, all fixed (section 5).

## 2. The `ohao` CLI (`tools/ohao/main.cpp`)

- Commands: `schema`, `validate`, `fmt`, `apply`, `query`, `summary` and `run`. The usage lines are in the header comment of `main.cpp`.
- Every command takes `--root DIR`, the project root that asset paths resolve against (default: the working directory). `--seed` is 16 hex digits, like an id.
- Output: one JSON document on stdout, `{"result": ...}` or `{"error": {code, message, data}}`. Stdout carries only JSON. The engine's logs go to stderr. `run --script` writes one line per operation.
- Order in `run`: `scene.load`, `--steps`, the script lines, `--render`, `--save`. Without `--script` the output is one document, `{"result": {"scene.load": …, "world.step": …, …}}`, with the operations that ran. The first failure stops the run and is printed as `{"method": …, "error": {…}}`. With `--script`, every line runs and the exit code is the worst one seen.
- Exit codes: 0 ok; 1 validation, usage or input error; 2 an asset or instantiate error (`E_ASSET_NOT_FOUND`, `E_ASSET_INVALID`, `E_MESH_INVALID`); 3 a GPU or internal error (`E_RENDER`, `E_INTERNAL`). Every other code gives 1. `validate` exits 2 only when every error is an asset error.
- `run --render` also takes `--mode deferred|rt_realtime|rt_offline`, `--spp`, `--frames`, `--denoise`, `--size`, `--post` and `--exposure`. Without `--post`, a deferred capture renders as `cornell_box` does.

## 3. Render host and `render.capture`

- `ohao_host` (`tools/ohao/CMakeLists.txt`, a static library) holds `RenderHost` (`tools/ohao/render_host.hpp`, `render_host.cpp`). It is the handler `api::Session::setRenderHandler` calls. The CLI and `restore_render_gpu_test` both use it.
- `render.capture` params: `{path, mode, spp, frames?, width, height, denoise, post, exposure?}`. Result: `{path, frames, ms, validation?}`.
- Defaults: `frames` is 10 for deferred and spp + 3 for path traced, as the examples render. `post` turns on the deferred effects it lists (`bloom`, `taa`, `ssao`); none are on by default. `exposure` defaults to the renderer's own value, 0.6. `validation` is the message count when the validation layers are on.
- Each capture applies the world's `Camera` and `Environment` (`tools/ohao/scene_host.hpp`), then renders per frame as the examples do: `world.update(0)`, then `render()`.
- Each later capture restarts accumulation, deferred included, so it renders as a fresh renderer would.
- `ohao_api` checks the params and stays GPU-free. `E_RENDER` covers no usable GPU, a failed frame and an image that could not be written. `E_INTERNAL` covers an exception that escaped. Both exit with 3.

## 4. Scenes, goldens and tests

- Checked-in scenes: `assets/scenes/cornell_box.scene.json` and `assets/scenes/damaged_helmet.scene.json`. The helmet file carries the baked `SceneFramer` room, camera and lights for `DamagedHelmet.glb`.
- Five `cli_*` scene goldens in `tests/golden/manifest.json`. Each runs `ohao run <scene> --render` on a checked-in scene and compares the image with the existing golden, with the existing tolerance:
  - `cli_cornell_box`: rt_offline, 16 spp, `--denoise none`;
  - `cli_damaged_helmet`: rt_offline, 16 spp, `--denoise none`;
  - `cli_cornell_box_deferred`: deferred, 10 frames, no post effects;
  - `cli_damaged_helmet_deferred`: deferred, 30 frames, `--post taa,ssao,bloom --exposure 1`. The scene file holds no render settings, so the command line states them;
  - `cli_cornell_box_rt_realtime`: rt_realtime, 16 spp.
- `tests/cli/check_cli.py` runs the 27 cases in `tests/cli/cases.json`. Each case runs twice from an empty `build/cli_tmp`. Both runs must match in exit code, stdout and written files, and the results must match `tests/cli/expected/` or a checked-in file. `--cpu-only` skips the 4 cases that create a device. Exit 3 is covered by a render to an unwritable path. Exit 2 is covered by `validate --check-assets` and by a render of a missing model. The two `*_save_scene` cases (test 16) are in the same table.
- `.gitattributes` keeps `tests/cli/**` at LF, like the other byte-compared goldens, so the expected stdout checks out as the CLI writes it.
- `tests/reflect/check_scene_schema.py` also runs `ohao schema` and checks that every checked-in scene passes the engine schema.
- `tests/renderer/restore_render_gpu_test.cpp` (test 17, section 6).
- `scene_bench --write-scene <path>` writes the bench10k scene, for timing the CLI.

## 5. Bugs found and fixed

- **Two VMA builds in one binary (crash).** `NRI_VK.lib` compiles its own copy of VMA from the same checkout, with dynamic function loading. Under `/FORCE:MULTIPLE` the linker keeps whichever copy it pulls first, and that depends on link order. The CLI got NRI's copy, which crashed in `vmaCreateAllocator` because no `vkGet*ProcAddr` was given. `ohao/gpu/vulkan/gpu_allocator.cpp` now uses NRI's configuration and always passes the two entry points, so either copy behaves the same.
- **Deferred `resetAccumulation` (restored image off by up to 14).** It reset only the path tracers. A deferred capture after another one kept RT GI's history and frame index, the clock behind RT GI's seeds and the sky, TAA's history, the previous view-projection, and the frame slot that indexes TAA's jitter. The restored image differed by max abs 14. Deferred mode now idles the device, resets the frame slot to 0 and calls `DeferredRenderer::resetTemporalState()`, as `resize()` does.
- **`render_golden.py` binary lookup.** It took `build/ohao`, which is the CMake binary directory of the `ohao/` tree, for the executable. It accepts files only now.
- **CLI stdout in text mode.** A duplicate of stdout keeps text mode, so the JSON came out with `\r\n`. The duplicate is now set to binary with `_setmode(_O_BINARY)`. Descriptor 1 points at stderr, so engine logs stay off stdout.
- **Exposure default.** The default is the renderer's own 0.6, not 1.0. `RenderHost` reads it from the renderer at startup, and a capture without `exposure` uses it.

## 6. Spec corrections

- Test 17 required the restore frame to upload 0 instance bytes. That cannot hold after a move, because the restore has to put the sphere's old transform back on the GPU. The test now checks two things instead. The restore frame uploads exactly what the move's frame uploaded: one instance, 160 bytes. A second, unchanged restore reports no change and its frame uploads 0 bytes.
- The spec's test 17 note records this as "Corrected in step 9".

## 7. Results

Release build, this machine (RTX 5070 Laptop GPU). `scene_bench` is single-threaded.

Unit and suite results:
- `engine_tests` 58/58, `runtime_tests` 21/21, `ecs_tests` 26/26, `scene_tests` 60/60, `reflect_tests` 31/31, `api_tests` 16/16, `force_generator_tests` 54/54. `physics_backend_tests` and `audio_system_tests` pass.
- `env_cdf_test` 2/2, `denoise_parse_test` 5/5, `sobol_test` 8/8, `renderer_test` passes.

GPU tests:
- `instantiate_gpu_test` 5/5. `scene_view_gpu_test` 7/7.
- `restore_render_gpu_test` 14/14:
  - plain (no post effects): the restored image has 0 differing bytes;
  - post effects on: max abs 1, the fresh-renderer floor;
  - instance bytes: the move 160, the restore 160, the unchanged restore 0;
  - validation: 0 messages.

CLI, schema and validation:
- `check_cli.py` 27/27. Exit codes 0, 1, 2 and 3 are all covered. Two runs are byte-identical.
- `check_scene_schema.py` 16/16.
- `check_validation.py` is clean.
- `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200`: exit 0, 0 messages, including teardown.

Render goldens, 10/10, tolerances unchanged. Max abs per image:
- example goldens: `cornell_box` 0, `damaged_helmet` 1, `cornell_box_deferred` 0, `damaged_helmet_deferred` 2, `cornell_box_rt_realtime` 2;
- CLI goldens: `cli_cornell_box` 0, `cli_damaged_helmet` 1, `cli_cornell_box_deferred` 0, `cli_damaged_helmet_deferred` 1, `cli_cornell_box_rt_realtime` 4.

`scene_bench --check` (bench10k medians, 50 rounds):

| Operation (bench10k) | Measured (ms) | Budget (ms) |
|---|---|---|
| `saveScene` pretty | 9.02 | 10 |
| `loadScene`, into an empty world | 12.77 | 20 |
| `scene.validate` | 8.12 | 10 |
| snapshot | 0.53 | 1 |
| restore, nothing changed | 0.59 | 1 |
| restore, 1% of rows changed | 0.48 | 2 |
| restore into an empty world | 2.24 | 10 |
| `world.apply`, batch of 100 `field.set` | 0.182 | 0.2 |
| `world.query`, 1,000 ids | 1.18 | 2 |

Scaling (bench100k against the paired bench10k samples): every ratio is between x1.3 and x11.8 and passes, except `restore_empty` at x13.5, which is over the x12 line. It is reported, not loosened. Restore was not changed in this step.

Recorded CLI wall times (process time, median; `scene_bench --write-scene` writes the bench10k file):
- `ohao validate` on bench10k: 43.0 ms;
- `ohao run --steps 1` on bench10k: 45.8 ms;
- Cornell load to first frame (`ohao run … --render … --mode deferred --frames 1`, renderer start included): 481 ms.

## 8. Known gaps and deferred

- The bench100k `restore_empty` ratio is x13.5, over the x12 line. It is not loosened. Budgets change only by Frank's ruling.
- With TAA, SSAO and bloom on, the deferred renderer is not bit-exact even between two fresh renderers (1 LSB on about 1.4k pixels). A long-lived renderer after a reset also differs from a fresh one, by 1 LSB on about 20k pixels. The suspected cause is a read before a write of an image that a fresh allocation zeroes. This was not investigated in this step.
- The untracked scratch file `examples/model_viewer_old.cpp` from step 8 is still in the working tree (this session cannot delete files). It is not committed; delete it by hand.

## 9. Running the new tests

```
build/Release/restore_render_gpu_test.exe
python tests/cli/check_cli.py                  # 27 cases, each run twice
python tests/cli/check_cli.py --cpu-only       # skips the 4 cases that create a device
python tests/cli/check_cli.py --only NAME      # one case
python tests/reflect/check_scene_schema.py
python tests/golden/render_golden.py tests/golden/manifest.json
build/Release/scene_bench.exe --check tests/bench/scene_budgets.json
build/Release/scene_bench.exe --write-scene <path>
build/Release/ohao.exe validate assets/scenes/cornell_box.scene.json --check-assets
OHAO_VALIDATION=1 build/Release/ohao_game.exe --frames 600 --resize-at 200
```
