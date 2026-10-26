# Level pipeline, step 5: the agent surface

**Spec:** `docs/specs/2026-10-26-level-pipeline-design.md` ("The agent-facing surface" and its "As built
(step 5)", "Spawn points", "Performance budgets" `level_info_bytes`; "Build order" step 5); plan
`docs/plans/2026-10-26-level-pipeline.md`, Task 5.

**Status:** built and tested. `tools/check.py --all` 43/46: every functional suite, the goldens and
`level_bench` pass; `scene_bench`, `serve_bench` and `render_bench` were noisy (pending a quiet rerun,
Frank's ruling).

## 1. Summary

**What step 5 delivers:**
- Four methods, on the CLI, `ohao serve` (native and MCP) and the window host alike:
  - `level.info {entity?}`: a level's summary in ~480 bytes (nodes, meshes, triangles,
    colliders by shape, bodies by motion, spawns in node order with their facing, warnings);
  - `level.reload {entity?}`: re-reads the file and rebuilds the level's entities and bodies
    in place; an unchanged file keeps `world.hash`;
  - `physics.raycast {from, dir, max?}`: the closest collider along a ray, ~100 bytes;
  - `physics.info {}`: bodies, shapes, build time and the collision warnings.
- `ohao level <file>`: `level.info` for a level file, without a scene or a GPU.
- The token budgets hold: `level.info` on the blockout 482 bytes (budget 1,024, now gated in
  `level_bench`), a ray 103 bytes (200), `tools/list` 19,756 bytes for 28 tools (24,576).

**What step 5 does not deliver:** the kit2k budgets and shape caching (step 6), stepping
dynamic bodies (sub-project 6), refreshing GPU meshes on `level.reload`.

## 2. What was built

**How the API reaches the level and the physics scene.** No `Host` change: both systems are
the World's, installed by `level::installLevels` and `ecs::installPhysics`, which leave
accessors on the World's context. `ohao/api/level.cpp` calls:
- `level::levelsInstalled`, `levelSummary`, `levelDiagnostics`, `reloadLevel`
  (`level/level_system.hpp`, `ohao_level_world`), and
- `ecs::physicsScene(world)` (`ecs/physics_scene.hpp`, `ohao_ecs`).

`ohao_api` now links `ohao_level_world`, which links only `ohao_level`, `ohao_ecs` and
`ohao_reflect`: the edge points down, and `reflect_tests`' dependency rules pass. A World
without the systems answers `E_STATE`; every OHAO host installs both. Each method first
runs the `level`, `transform` and `physics` sync systems (never instantiate or the render
extract), so a query right after `scene.load` or `world.apply` sees the nodes and bodies.

**Level system** (`level/level_system.{hpp,cpp}`):
- `LevelSummary{path, loaded, nodes, meshes, uniqueMeshes, lights, cameras, triangles,
  loadMs}` per level, from the file's structure and the last expansion's time.
- `reloadLevel`: forgets the file's structure and the resolver's meshes of it
  (`AssetResolver::forget`, new; `FileAssetResolver` drops every `(file, mesh)` entry),
  marks the level dirty, and after the re-expansion touches the nodes' colliders so the
  physics system rebuilds their bodies.

**API** (`api/level.cpp`, `params.{hpp,cpp}`, `session.cpp`):
- Params structs `LevelInfoParams`, `LevelReloadParams` (`entity`, a 16-hex-digit id),
  `PhysicsRaycastParams` (`from` m, required; `dir` unitless, required; `max` m, default
  1000), `PhysicsInfoParams`, with docs and units.
- `level.info`: `{entity, path, nodes, meshes, uniqueMeshes, triangles, colliders,
  bodies, spawns: [{tag, entity, pos, yawDeg}], moreSpawns?, lights, cameras, warnings,
  moreWarnings?, errors?, loadMs}`. Spawns in node order (the first 8), `yawDeg` the facing
  about +Y from −Z to 0.1°, positions to 0.1 mm. Warnings: the level's hint diagnostics,
  `W_LEVEL_NO_PLAYER_SPAWN` (new), its nodes' physics warnings (the first 4). A file that did
  not load answers with its error (`E_ASSET_NOT_FOUND`: exit 2).
- `physics.raycast`: `{hit, entity, name, point, normal, dist}` or `{hit: false}`; the name
  cut at 40 bytes, point and normal to 1e-4.
- `physics.info`: `{bodies, shapes, unstepped, buildMs, warnings?}`.
- Errors: `E_STATE` without the systems; `E_NOT_FOUND` (new) for no level, or an entity
  that is not one (at `/entity`); `E_PARAMS` (new, JSON-RPC -32602) for a zero `dir` or `max`
  outside (0, 100000]; several levels without `entity` is `E_COMMAND`, naming them.
- The MCP `initialize` instructions mention `level_info` and `physics_raycast` (591 of 600
  characters).

**CLI** (`tools/ohao/main.cpp`): `ohao level <file> [--root DIR]` loads the file as the only
`Level` (entity `0000000000000001`) of an empty world and prints `level.info`.

**Harnesses:**
- `check_cli.py`: a case's `mask` keys (timings) compare as 0; cases `level`,
  `level_missing` (exit 2), `level_usage` (exit 1).
- `check_serve.py`: the goldens hold `loadMs` and `buildMs` as 0; the new tools' examples; a
  session `tests/serve/sessions/level.jsonl`.
- `e2e_mcp.py`: after the replay, the blockout: `level_info`, a ray down at the player
  spawn, `physics_info`; 28 tools.
- `level_bench`: `level_info_bytes` (a size, so a noisy machine cannot excuse a miss);
  `currentStep` 5.

**Docs:** the serve guide's "Load a level" (with a run script, `docs/agents/examples/level.jsonl`),
the CLI guide (`ohao level`, the four methods, the codes, 28 tools), AGENTS.md (suites, a
level recipe in the levels notes), ARCHITECTURE.md ("Recipe: add a level"; the library table
brought up to date: `ohao_level`, `ohao_level_world`, the ECS's physics edge, the API's level
edge), the monograph's module map, the spec and plan as built.

**Tests:** `api_tests` (7 new, 51 in all): `sp5_t08_level_info_counts_and_spawns`,
`_level_info_is_small`, `_raycast_hits_floor`, `_raycast_params_refused`,
`_physics_info_counts`, `_level_reload_keeps_world_hash`,
`_warnings_reach_level_and_physics_info`; the method count is 28.

**Goldens regenerated** (looked at; only what the step adds): the engine and fixture
catalogues and scene schemas (the three codes), the CLI expectations (schema hash
`d3d8dddea5a7226d` → `e9e1c34d831dc812`, `level` in the command list, 16 more methods in an
`E_METHOD` hint, the three new cases), the serve expectations (the hash, the instructions,
the new `level.jsonl`). No render golden changed.

## 3. Bugs found and fixed

- None in existing code. In my own tests while building: spawns are in the file's node order,
  which Blender writes by name (enemies before the player); the test now asserts that order.

## 4. Spec corrections and rulings

- **Host access:** the plan named "`Host` access to the world's `PhysicsScene`". The scene is
  the World's (step 4's `installPhysics`), so the session reads it from the World
  (`ecs::physicsScene`); `api::Host` stays the renderer's interface.
- **Error codes as the plan names them:** `E_NOT_FOUND` and `E_PARAMS` are new catalogue
  codes rather than the existing `E_ENTITY_NOT_FOUND` / `E_RANGE`. So `max` carries no field
  limits in its schema (its range is in its doc and is checked by the method), and a zero
  `dir` and an out-of-range `max` give the same code.
- **Queries sync first:** `scene.load` runs only the `level` sync; the four methods run
  `level`, `transform` and `physics` before answering. After a `world.apply` that moved
  something, this lets transforms catch up before the next render's sync would, so that
  render's motion vectors for the moved entities are zero (captures render from a reset, so
  no test or golden moved).
- **Spawns in node order** (spec), the first 8; warnings the first 4. `W_LEVEL_NO_PLAYER_SPAWN`
  added as the spec names it.
- **Timings in goldens:** `loadMs` and `buildMs` vary run to run; the serve and CLI harnesses
  compare them as 0 and keep everything else byte for byte.
- **`level.reload` and the GPU:** meshes already uploaded stay (instantiate caches per
  `(file, mesh)`); the reload re-reads structure, components and collision geometry.

## 5. Results

| Gate | Result |
|---|---|
| `tools/check.py --all` | 43/46: every functional suite passed; `scene_bench`, `serve_bench`, `render_bench` noisy |
| `api_tests` | 51/51 (7 new) |
| `engine_tests` / `scene_tests` / `ecs_tests` / `reflect_tests` | 85/85 / 84/84 / 37/37 / 42/42 |
| `level_physics_tests` / `physics_backend_tests` | 14/14 / 61/61 |
| `rpc_tests` | 18/18 |
| `check_cli` / `check_serve` / `e2e_mcp` | 32/32 (28/28 CPU) / 190/190 / 29/29 |
| `check_live` / `check_validation` | 42/42 / 6/6 |
| render goldens | 25/25, every one unchanged |
| `check_docs` | 80/80 |

**Sizes:** `level.info` on the blockout 482 bytes (481–482 depending on the level's id),
budget 1,024; a ray 103 bytes, budget 200; `tools/list` 19,756 bytes, 28 tools, cap 24,576.

**A ray**, down at the player spawn (103 bytes):
`{"hit":true,"entity":"fb31dd4d0994b3f6","name":"Floor","point":[1.5,0,2.2],"normal":[0,1,0],"dist":0.5}`

**Benchmarks.**
- **`level_bench`** passed on check.py's rerun (other processes 40% of a core, calibration
  x1.03): `level_info_bytes` 482 (budget 1,024); `blockout_load_ms` median 10.841 ms, p95
  15.859 (budget 20). Its first gate run was noisy.
- **Noisy, pending a quiet rerun** (Frank's ruling). Other processes used 0.6–6.1 cores while
  they ran. No budget was changed.
  - `scene_bench`: three runs, all noisy. `save_pretty` on bench10k was 12.060 / 12.683 /
    10.026 ms in clean rounds against 10 ms; the quietest run (0.6 cores, calibration x1.02)
    is the 10.026. Step 4 measured 9.318–9.368 after its writer fix. This step changes nothing
    the bench runs: no saved type, nothing under `ohao/scene` or the writer. The first run
    also had `apply_100_field_set` at 0.210 ms against 0.2; the rerun had 0.172.
  - `serve_bench`: noisy in all three runs (calibration up to x1.67, 1.4–5.0 cores); every
    measurement it printed was inside its budget.
  - `render_bench`: `cornell_720p lighting` 0.366–0.376 ms against 0.32, the standing miss
    step 4 recorded for Frank's ruling. This step touches no renderer code or shader.

**Samples** in `C:\Users\djmax\ohao_refs\look_shots\sp5_step5\` (real runs):
- `ohao_level_blockout.txt`: `ohao level assets/levels/blockout_room.glb`, exit 0, 493 bytes.
- `serve_level_session.txt`: `ohao serve` on the blockout scene: `scene.load`, `level.info`,
  three rays (the spawn, the doorway, the ramp: normal (0, 0.9806, 0.1961)), a zero `dir`
  (`E_PARAMS`), `physics.info`, `world.hash`, `level.reload`, `world.hash` (the same).
- `raycast_top_api.png`: the blockout from above, one `physics.raycast` through `ohao serve`
  per 4 cm pixel (33,600 rays, all hits), tinted by entity, shaded by normal and height; the
  thin dark seams are rays landing exactly on a box's side plane (the step's edge at
  x −1.5, the doorway jamb at x 0.9), which return that face's normal.

## 6. Known gaps

- **`level.reload` keeps GPU meshes:** a mesh whose geometry changed on disk keeps its old
  upload until no entity uses it.
- **The early transform sync** above (motion vectors of the next render after an edit and a
  level or physics query).
- **Spawns and warnings are capped** in `level.info` (8 and 4); `world.query {has:
  ["SpawnPoint"]}` and `physics.info` give the rest.
- Step 6: kit2k budgets, shape caching, one Assimp import per file.
- **Benchmarks** need a quiet-machine rerun (`python tools/check.py --labels bench`).

## 7. How to run

```
build/Release/api_tests.exe
build/Release/ohao level assets/levels/blockout_room.glb
python tests/serve/check_serve.py
python tests/cli/check_cli.py --cpu-only
python tests/serve/e2e_mcp.py
build/Release/level_bench.exe --check tests/bench/level_budgets.json
```
