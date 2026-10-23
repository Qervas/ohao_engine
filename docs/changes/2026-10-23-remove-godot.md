# Remove the Godot remnants

**Spec:** none; this is a cleanup ordered directly by Frank, 2026-10-23 15:00 Berlin: "remove godot, we do our own."
**Status:** Done.

## 1. Summary

OHAO is its own engine. The Godot GDExtension host, its editor and its SCons build were deleted on 2026-04-08 (commit 5bf6615f); this change removes what that left behind in tracked files. Historical change records, specs and bug write-ups stay as written; history is not rewritten.

## 2. What was removed

- `.gitignore`: the Godot block (godot_editor/project/.godot/, godot_editor/.sconsign.dblite, godot_editor/bin/, *.import, godot_editor/*.a, shaders/*.a, godot_editor/external/, shaders/external/), all added together in b114aed3 for the Godot/SCons layout; none matches anything now, and `*.a` is still ignored by the global rule. The shelved-work note above `_backup/` no longer names a "Godot-era editor"; `_backup/` stays ignored.
- `shaders/compile_shaders.sh`: deleted. It compiled into build/shaders and copied every output into godot_editor/project/bin/shaders (recreating that tree). The CMake `shaders` target replaces it entirely: the script walked a fixed list (core shadow postprocess compute debug), skipped rt/, particles/ and overlay/, used glslangValidator without --target-env, and failed on shaders/postprocess/ssr.comp, which the CMake build compiles. Nothing referenced the script except the monograph.
- `assets/fonts/fa-solid-900.ttf` and `assets/fonts/README.md`: deleted, with the copy step and the `assets/fonts` directory creation in the top-level `CMakeLists.txt` (`copy_assets` now copies only the Cornell box OBJ/MTL). The font was added in 514b71fa for the ImGui editor's viewport toolbar; ImGui was removed in 4e599907, when the README was changed to "Standard fonts are bundled with Godot Editor". No code loads the font.
- `tests/renderer/CMakeLists.txt`: the header comment "Run the Vulkan renderer WITHOUT Godot" is replaced by a plain description of renderer_test.
- `CHANGELOG.md`: the current entry no longer says "(no Godot host)"; the archived [0.2.0] Godot GDExtension entry keeps its history but now says the host, its editor and its SCons build were deleted on 2026-04-08 (5bf6615f), instead of calling it "no longer the primary product line".
- Monograph: `site/content/units/shaders/layout.md` loses the section "The other compiler, still in the tree" (it described compile_shaders.sh and its Godot copy); `site/tools/monograph/tree/shaders.py` no longer lists the script as a unit file. Hand-written hub pages: `site/m/audio.html` drops "Godot host docs: synced automatically" (now: call setListenerPosition every frame from the camera; nothing in the engine calls it yet); `site/m/build.html` names the `shaders` CMake target instead of compile_shaders.sh. The site rebuild also refreshed citation line numbers in five generated pages (deferred/post, shaders/compute, systems/build, systems/status, systems/tests) because the edited files shifted.

## 3. Remaining mentions and why they stay

Remaining hits of `git grep -i 'godot\|scons'`:
- `CHANGELOG.md:78` and `:80`: the archived [0.2.0] entry; history.
- `docs/bugs_solved/001_staging_buffer_resize.md:5`: a bug record from the Godot era; history.
- `docs/changes/2026-10-23-reflect-step10.md:45` and `:111`: the step 10 change record, on why the skills waited outside `.claude/` and on the stale Godot/scons skills they replaced; history.
- This record itself; history.
- `docs/specs/2026-10-22-reflection-scene-serialization-design.md:18`: names Godot as a third-party engine in the design's comparison ("Today's engines (Godot, Unity, Unreal)"); factual third-party mention.
- The same spec, `:1274`, `:1306`, `:1560`: the spec's record of the stale Godot/scons skills and the step that replaced them; history.
- `external/cmake/jolt.cmake:2` and `ohao/physics/backend/jolt/jolt_backend.hpp:27`: "Jolt is used by Godot 4.x"; factual third-party mention, kept by Frank's instruction.
- False positives of the substring "scons" inside other words: `ohao/render/deferred/csm_pass.cpp:412` (rasterizer.depthBiasConstantFactor), `ohao/render/rt/rt_acceleration_structure.cpp` and `.hpp` (asConsumerStages, m_asConsumerStages), `tests/shadow_system/shadow_system_tests.cpp:239` and `:317` (testShaderBindingsConsistency).
- No tracked file name contains godot or scons. `.claude/` (untracked, not touched) has no mention either; the skills installed there are the refreshed ones.
- Untracked or ignored Godot folders on disk: none. There is no godot_editor/ and no _backup/ in the working tree.

## 4. Results

All from runs today, after the change.

- Re-configure and Release build (-j6): no errors.
- `tests/docs/check_docs.py`: 70/70 passed, 16 skipped (the skipped ones are the builds and the slow gates, run separately below). This runs every suite: runtime, reflect, scene, api, systems, physics_backend, audio_system, force_generator, env_cdf, denoise_parse, sobol, check_cli (CPU and GPU), check_scene_schema, engine, ecs, renderer_test, instantiate_gpu_test, scene_view_gpu_test, restore_render_gpu_test, check_validation, ohao_game under OHAO_VALIDATION=1 with a resize, the CLI guide's commands, check_unit reflection/api and test_monograph_structure.
- `check_unit shaders/layout`: PASS, 24 citations.
- Render goldens: 10/10 pass at unchanged tolerances. Max abs per image: cornell_box 0, damaged_helmet 1, cornell_box_deferred 0, damaged_helmet_deferred 1, cornell_box_rt_realtime 2, cli_cornell_box 0, cli_damaged_helmet 1, cli_cornell_box_deferred 0, cli_damaged_helmet_deferred 1, cli_cornell_box_rt_realtime 2. The shader build did not change (the deleted script was never part of it).
- `scene_bench --check`: the first run failed seven bench10k budgets, with every operation about 2.5x slower than earlier the same day (save 24.5 ms against 9.4 ms), including operations whose code this change does not touch. The second run, immediately after, passed every budget and every ratio: bench10k save 9.360, load 13.367, validate 8.479, snapshot 0.539, restore_unchanged 0.615, restore_1pct 0.495, restore_empty 2.024, apply 0.157, query 1.210 ms; restore_empty ratio x7.3.

## 5. Known gaps

- The first scene_bench run's slowdown was not diagnosed (process inspection was not available in the session). No code the benchmark runs changed.

## 6. Running the checks

```
git grep -n -i 'godot\|scons'
python tests/docs/check_docs.py
python tests/golden/render_golden.py tests/golden/manifest.json
build/Release/scene_bench.exe --check tests/bench/scene_budgets.json
```
