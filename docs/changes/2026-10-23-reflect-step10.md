# Sub-project 3, build step 10: agent guide, recipes, spin system and entity.duplicate

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 10; "Engine development by agents (G2)" with its "As built (step 10)" bullets; the `entity.duplicate` command details; "Budgets (gated)", the step 10 bench note)
**Status:** Done. Every suite passes and every bench10k budget passes. The bench100k `restore_empty` ratio was over its line in the second of two runs (section 8). The skills are written but not yet installed into `.claude/skills/` (section 4).

## 1. Summary

Step 10 writes the guide an agent needs to work on the engine, and tests every command that guide shows. It adds `AGENTS.md` (with a one-line `CLAUDE.md` that imports it), `docs/ARCHITECTURE.md` with four recipes, the `ohao` CLI guide, three skills, a monograph module and a doc-command checker, `tests/docs/check_docs.py`. The four recipes were each written and then followed once with a real feature: the `Spin` component, the spin system, the `entity.duplicate` command and the `systems_tests` suite. The roadmap row is marked done.

Commits of this step: `26ffd78` (the features and the suite), `84299a5` (the docs, the checker and two CLI fixes), `89000ec` (the monograph module and citation repairs). This record and the spec and roadmap update are committed after them.

## 2. The recipes and what following them produced

- **Add a component: `Spin`.** `Spin` (`ohao/ecs/components.hpp`) turns an entity about a local axis at `degreesPerSecond`. It is registered with docs, a new unit `deg/s` (added to `kUnits` in `ohao/reflect/registry.cpp`), a `uiRange` of -720 to 720 (`ohao/ecs/scene_types.cpp`), and a per-type writer (`ohao/ecs/scene_writers.cpp`). It is covered by the completeness, authoring-split and random-world checks in `scene_tests` and by error-corpus case `19_spin`. The catalogue and schema goldens were regenerated for the new type, unit and schema hash. The two CLI expected outputs that carry the `schemaHash` changed in that hash only.
- **Add a system: the spin system.** This is the first engine-provided system. `updateSpin` (`ohao/ecs/spin_system.cpp`) post-multiplies each entity's rotation by `angleAxis(radians(degreesPerSecond * dt), axis)`, with the axis normalized in the entity's own frame, and writes through `patch<Transform>`. It is built into `World` and runs before `transform`, so the change reaches the transform system and the render extract in the same update. It is in the t13 dependency list. When `dt`, the rate or the axis is zero or not finite, it writes nothing: no `TransformDirty`, nothing re-uploaded, and headless `update(0)` renders are unaffected.
- **Add an API command: `entity.duplicate`** (`ohao/api/commands.cpp`).
  - Copies the entity and, by default, its subtree, with fresh ids and creation order, every saved and preserved component.
  - Places the copy under `parent` (default: the source's parent) and names it `name` (default: the source's name).
  - Parent references inside the copied subtree point at the copies; any other reference keeps its target.
  - Refuses a subtree that holds a singleton (`E_SINGLETON`) or a parent inside the copied subtree (`E_PARENT_CYCLE`).
  - Its inverse is `entity.destroy` of the copy.
  - Each value is copied before it is emplaced, because `ComponentOps::emplace` moves from its argument and would empty the source's strings and vectors. A `static_assert` ties `kCommandFns` to `kOps`.
- **Add a test suite: `systems_tests`** (`tests/systems/systems_tests.cpp`, 6 tests). Spin runs before the transform system; a turn about the local axis by rate times dt, checked bit for bit; still entities stay untouched (dt zero, zero rate, zero or NaN axis, infinite rate); negative rates undo a turn and steps add up; determinism over 600 steps, with a child following its spinning parent; and a `world.step`, snapshot and restore pass through the API. It ends with the one-line JSON summary.

## 3. Docs

- **`AGENTS.md`** is the entry point. It covers:
  - the build, with the scoop `cmake` path, `-j6` at most, and the manual `shaders` target;
  - every gate, with its pass criterion (exit code 0; the newer suites also end with a JSON line), split into CPU suites, GPU suites and harnesses;
  - the golden, validation and bench harnesses;
  - the gotchas, moved in-repo from the private notes;
  - the conventions: spec, plan, build; the change record shape; commits; line endings; forward-only docs; the performance gate; test naming.
- **`CLAUDE.md`** is one line, `@AGENTS.md`. It is no longer git-ignored.
- **`docs/ARCHITECTURE.md`** has the library table with dependency directions, the dependency rules, the frame as the code runs it (spin, instantiate, transform, render_extract, then `recordUpdates`), and the four recipes. The frame flow in the spec had instantiate after transform; the code has run it before transform since step 7, and the spec is corrected (section 6).
- **`docs/agents/ohao-cli.md`** is the CLI contract: every command with real output, the methods, the edit commands, the components, the error codes and habits that save tokens. Each example is checked by the doc-command checker.
- **`tests/docs/check_docs.py`** runs the `console` blocks of `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/agents/ohao-cli.md` and the skills.
  - A `$ ` line is a command, run with Git Bash from the repo root. The lines after it are the expected stdout, compared exactly with trailing spaces ignored. A line that is only `...` matches any run of lines.
  - `# exit N` expects that exit code. `# slow` marks builds, the golden corpus and the bench gate. `# gpu` marks commands that need a device; `--cpu-only` skips them.
  - Each file starts from an empty `build/doc_tmp`, and its commands run in order.
  - Its last line is the JSON summary `{"suite": "doc_commands", ...}`.

## 4. Skills

- Three skills are written: `build` (configure, build, shaders), `verify` (the full gate set with pass criteria) and `add-component` (the recipe in section 2). Every command in them is run by `check_docs.py`.
- They are in `docs/agents/skills/`, not `.claude/skills/`, because the session that wrote them could not write or delete under `.claude/`. The install steps are in `docs/agents/skills/README.md`. They delete the stale Godot and scons skills, including `_disabled/`, which the forward-only rule requires.
- `.gitignore` now tracks `.claude/skills/`. The rest of `.claude/` stays local.

## 5. Monograph

- A new module, "12 · Reflection & agent API", with seven units: registry, scene-files, load-validate, snapshots, api, instantiate, cli.
- Existing units corrected: scene/world, architecture/request-path, architecture/module-map, scene/render-extract, and the units under systems, status, tests, build and examples.
- Sixteen citations that steps 7 to 9 had broken were repaired.
- Four path-tracer and denoise pages claimed that ReSTIR bindings 29 to 35 were unwritten. They no longer do.
- The site has 92 leaf pages in 17 modules. `test_monograph_structure` passes 16 tests, and `test_blocks` and `test_cite` pass.

## 6. Bugs and inaccuracies found and fixed

- **Git Bash rewrites arguments that start with `/`** into Windows paths. `ohao query --fields` therefore takes `Type/field`, without the leading slash.
- **`ohao apply` had no `--seed`.** The ids of created entities were random. `--seed` is now accepted, and two CLI cases cover it.
- **`world.summary`'s documented result named a `truncated?` member that was never written.** The doc now names `moreRoots`, which is what the command writes.
- **The `E_ASSET_INVALID` message and the `ModelAsset` doc named 4 of the 8 model formats the loader accepts.** Both name all eight now (catalogue goldens: the doc text and the hash).
- **The comment in `entity.duplicate` gave the wrong reason for copying values before emplacing.** It said a pool may grow and move its values; EnTT's storage is paged, so it does not. The real reason is that `ComponentOps::emplace` moves from its argument. Corrected.
- **AGENTS.md said never to run the monograph's Python from `site/`.** That holds for its tests; the site build must run from `site/`. Corrected, with the commands.
- **The spec's frame data flow had instantiate after transform.** The code runs instantiate before transform (since step 7), so a mesh it builds gets its world matrix in the same update. The spec is corrected and marked "corrected in step 10".

## 7. Results

Release build, this machine (RTX 5070 Laptop GPU). `scene_bench` is single-threaded.

Unit and suite results:
- `engine_tests` 58/58, `runtime_tests` 21/21, `ecs_tests` 26/26, `reflect_tests` 31/31, `scene_tests` 60/60, `api_tests` 17/17, `systems_tests` 6/6, `force_generator_tests` 54/54. `physics_backend_tests` and `audio_system_tests` pass.
- `env_cdf_test` 2/2, `denoise_parse_test` 5/5, `sobol_test` 8/8, `renderer_test` passes.

GPU tests:
- `instantiate_gpu_test` 5/5, `scene_view_gpu_test` 7/7, `restore_render_gpu_test` 14/14.

CLI, schema, docs and validation:
- `check_cli.py` 29/29.
- `check_scene_schema.py` 16/16.
- `check_docs.py`: 68/68 non-slow commands, CPU and GPU, across all six files. `check_docs.py --slow AGENTS.md`: 31/31, the slow ones included (configure, build, shaders, the golden corpus, the bench gate — which passed in that third run — the site build and the nested doc check).
- `check_validation.py` is clean.
- `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200`: exit 0, 0 messages, including teardown.

Render goldens, 10/10, tolerances unchanged. Max abs per image:
- example goldens: `cornell_box` 0, `damaged_helmet` 1, `cornell_box_deferred` 0, `damaged_helmet_deferred` 2, `cornell_box_rt_realtime` 5;
- CLI goldens: `cli_cornell_box` 0, `cli_damaged_helmet` 1, `cli_cornell_box_deferred` 0, `cli_damaged_helmet_deferred` 1, `cli_cornell_box_rt_realtime` 3.

`scene_bench --check`, two runs. Medians in ms, p95 in brackets from the first run. The second run's medians are recorded for three rows only.

| Operation (bench10k) | Median, run 1 [p95] | Median, run 2 | Budget (ms) |
|---|---|---|---|
| `saveScene` pretty | 9.154 [10.143] | 9.043 | 10 |
| `loadScene`, into an empty world | 13.121 [14.697] | | 20 |
| `scene.validate` | 8.329 [8.870] | 8.169 | 10 |
| snapshot | 0.523 [0.716] | | 1 |
| restore, nothing changed | 0.655 [0.860] | | 1 |
| restore, 1% of rows changed | 0.497 [0.552] | | 2 |
| restore into an empty world | 2.254 [2.495] | | 10 |
| `world.apply`, batch of 100 `field.set` | 0.185 [0.239] | 0.180 | 0.2 |
| `world.query`, 1,000 ids | 1.190 [1.323] | | 2 |

Every bench10k budget passes in both runs.

Scaling (bench100k against the paired bench10k samples):
- Run 1: every ratio passes, between x9.9 and x11.1. `restore_empty` is x10.7. The gate passes.
- Run 2: `restore_empty` is x14.9, over the x12 line, and the gate fails. Every other ratio passes in both runs.
- The 100k `restore_empty` medians were 24.7 and 24.8 ms; the paired 10k medians were 2.30 and 1.66 ms. The ratio crosses its line on the noise of the small denominator, not on a change in the work. It is reported, not loosened.

## 8. Known gaps and deferred

- **The skills are not installed.** Installing them needs the steps in `docs/agents/skills/README.md`, which also delete the stale Godot and scons skills.
- **The bench100k `restore_empty` ratio sits on its line:** x10.7 in run 1, x14.9 in run 2 (section 7); a third gate run, inside the doc check, passed. Budgets change only by Frank's ruling.
- **Two untracked scratch files** need deleting by hand, because this session cannot delete files: `examples/model_viewer_old.cpp` and `tests/golden/cornell_box_deferred.actual.png`. Neither is committed.
- **Open finding carried from earlier steps:** with TAA, SSAO and bloom on, deferred renders are not bit-exact run to run (section 8 of the step 9 record).

## 9. Running the new checks

```
build/Release/systems_tests.exe
python tests/docs/check_docs.py                  # every non-slow console command in the agent docs
python tests/docs/check_docs.py --cpu-only       # skips the # gpu commands
python tests/docs/check_docs.py --slow           # adds the builds, goldens and bench gate
python tests/cli/check_cli.py                    # 29 cases, each run twice
python tests/cli/check_cli.py --cpu-only
python tests/cli/check_cli.py --only NAME        # one case
python tests/reflect/check_scene_schema.py
```

The monograph's checks run from the repo root; the site build is the one command that runs from `site/` (AGENTS.md, gotchas):

```
python site/tools/check_unit.py reflection/api
cd site && python -c "from tools.monograph.build import build; build()"
python site/tests/test_monograph_structure.py
python site/tests/test_blocks.py
python site/tests/test_cite.py
```
