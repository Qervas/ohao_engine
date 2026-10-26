# Level pipeline, step 4: collision

**Spec:** `docs/specs/2026-10-26-level-pipeline-design.md` ("Collision generation", its "As built
(step 4)", "Performance budgets" `blockout_load_ms`; "Build order" step 4; tests 6 and 7, and test
9's grounding ray); plan `docs/plans/2026-10-26-level-pipeline.md`, Task 4.

**Status:** built and tested. Every functional suite passes. The benchmarks were noisy and wait
for a quiet-machine rerun (Frank's ruling of 2026-10-26). `render_bench`'s `cornell_720p
lighting` is over its budget in every run, noisy or not. This step did not cause it (section 5).

## 1. Summary

**What step 4 delivers:**
- `Collider` and `RigidBody`, saved components that scene files, commands and level hints all
  write. A level's mesh nodes collide by default (`col` defaults to `mesh`); `col`, `body`,
  `mass`, `fric` and `rest` now become components.
- The `physics` sync system, right after `transform`: one Jolt body per entity with a
  `Collider`, its shape fitted to the entity's mesh at its world scale (box, sphere, capsule,
  convex hull, triangle mesh), static unless a `RigidBody` says otherwise. A still world costs
  nothing; a moved body follows its entity without a rebuild.
- `UCX_` hulls joined into one compound body on the node they belong to (`ShapeInfo::COMPOUND`
  in the backend, a Jolt `StaticCompoundShape`).
- Ray casts against the scene (`PhysicsScene::raycast`, hits naming their entity). The blockout's
  spawns stand on its floor, step and ramp to the millimetre, its doorway lets a ray through,
  and its two crates come to rest at their authored heights when the backend is stepped.
- `Level.collision: false` (a render-only level) builds nothing.

**What step 4 does not deliver:** stepping in the world (dynamic bodies are built, never
stepped, until sub-project 6), `physics.raycast` and `physics.info` as API methods (step 5), the
kit2k budgets and shape caching (step 6).

## 2. What was built

**Backend** (`ohao/physics/backend`):
- `ShapeInfo::Type::COMPOUND` with `children` (`CompoundChild{shape, position, rotation}` in the
  body's frame), built by `createJoltShape` as a `StaticCompoundShapeSettings`.
- The Jolt temp allocator is `TempAllocatorImplWithMallocFallback` (10 MB, then the heap). See
  section 3.

**Components** (`ecs/components.hpp`, registered with docs, units and limits in `scene_types.cpp`,
per-type writers in `scene_writers.cpp`):
- `Collider{shape = Box, size = 0, offset = 0}` (saved; a zero size fits the shape to the mesh);
- `RigidBody{motion = Static, mass = 10 kg (≥ 0.001), friction = 0.6, restitution = 0}` (saved;
  friction and restitution in [0, 1]);
- runtime: `ColliderPart{body}` (a hull's body entity), `CollisionMesh{source}` (the mesh of an
  undrawn node), `PhysicsBody{handle}`, the `PhysicsMoved` tag;
- the `ColliderShape` and `Motion` enums (step 3 declared them) are registered.

**`ecs/physics_scene.{hpp,cpp}`:**
- `fitCollider(MeshData, ColliderShape, scale)`: box = the mesh's AABB (centre as offset) with
  the scale baked in; sphere = the AABB centre and the farthest vertex; capsule = along the
  AABB's longest axis, the radius the larger other half extent; sphere and capsule scaled by the
  largest axis (`roundedScale` when the axes differ by over 1%); convex and mesh = the scaled
  vertices; a mirrored scale (negative determinant) swaps two corners of each triangle so the
  mesh faces out.
- `PhysicsScene`: `sync`, `raycast(from, dir, max)` → `RayHit{entity, point, normal, dist}`,
  `info()` → `PhysicsInfo{bodies by motion, shapes by kind, buildMs, unstepped}`, `backend()`,
  `bodyOf(e)`, `warnings()`, `bodyBuilds()`.
- Dirty state: signals on `Collider`, `RigidBody`, `ColliderPart`, `CollisionMesh`,
  `MeshSource` and a destroyed `PhysicsBody` queue entities; the transform system tags
  `PhysicsMoved` on entities with a body or a hull. A sync rebuilds the touched bodies in
  creation order, moves the moved ones (`setPosition`/`setRotation`), and rebuilds a body
  whose world scale changed or whose hulls moved.
- A dynamic `Mesh` collider is built `Convex` with `W_LEVEL_DYNAMIC_MESH`; a collider with no
  size and no mesh gets `W_COLLIDER_NO_MESH`; uneven scale on a sphere or capsule
  `W_COLLIDER_SCALE`. Each is logged once per entity (`physics` channel) and listed by
  `warnings()`. The three codes are in the catalogue.
- The backend is created at the first body: Jolt, no worker threads, room for 65,536 bodies.
- `installPhysics(world, assets)` adds `physics` after `transform` (once per World);
  `physicsScene(world)` finds it. The CLI context and `LiveHost` install it beside
  `installLevels`.
- `instantiate.hpp` exposes `primitiveModel(MeshSource)`, so primitives can carry colliders.

**Level system** (`level/level_system.cpp`): `col` (or its default) becomes `Collider{shape}`;
`body`/`mass`/`fric`/`rest` become a `RigidBody` (only when one is given); an undrawn node with a
collider gets `CollisionMesh{asset, "#<mesh>"}`; a `UCX_<target>[_NN]` node gets
`ColliderPart{<target>}` (the name as written, else without its `_NN`). With
`Level.collision: false` none of these is set, passed-through `Collider`/`RigidBody` included,
and turning it on (or off) re-expands in place.

**Elsewhere:**
- `ohao_ecs` links `ohao_physics` (section 4).
- `scene_io`: the scene writer looks up per entity only the saved types some entity has
  (section 3).
- `tools/bench/level_bench.cpp` (minimal, `--only blockout`) and `tests/bench/level_budgets.json`
  (the spec's six budgets, `currentStep` 4), registered as a `bench` test.
- `tests/level/make_physics_fixture.py` writes `tests/level/fixtures/level_physics.gltf` (real
  geometry in a data URI; byte for byte reproducible).
- `reflect_tests`' source lints cover `physics_scene.{hpp,cpp}` (no reflection, no clock but
  the allowlisted `buildMs` statistic).
- Docs: AGENTS.md (the suite, the bench, a collision gotcha), the CLI guide (the components, the
  codes), the serve guide (the fly example's hash), the monograph's module map (the ECS links
  physics), the spec and plan as built.

**Tests:**
- `level_physics_tests` (new, label `quick`, 14 tests): test 6 on `level_physics.gltf` —
  `t06_fit_box_sphere_capsule_from_mesh`, `_convex_hull_of_mesh`, `_static_mesh_shape`,
  `_ucx_hulls_are_one_compound_body`, `_dynamic_mesh_becomes_convex_with_warning`,
  `_non_uniform_scale_baked`, `_mirrored_node_collides_outward`; test 7 on the blockout —
  `t07_ray_down_at_each_spawn_hits_authored_height` (±1 mm), `_ray_through_doorway_passes`,
  `_ray_beside_doorway_hits_wall_south_a` (z 3.0, normal −z), `_ray_on_ramp_returns_slope_normal`,
  `_crates_come_to_rest_when_stepped` (120 steps of 1/60 s), and two more:
  `t07_render_only_level_builds_no_bodies` (collision off, on, the level destroyed) and
  `t07_still_world_does_no_work` (10 syncs build nothing; a moved node's body follows it without a
  rebuild). `--look DIR` writes the ray pictures below.
- `physics_backend_tests` (2 new, 61 in all): `Single-threaded backend with 65,536 max bodies
  steps a box onto a floor` (the regression test of section 3) and `Compound shape: children
  placed in the body's frame`.
- `scene_tests`: the completeness, saved/runtime lists and the random worlds (colliders and
  rigid bodies, every shape and motion) cover the new components.

**Goldens regenerated** (looked at; only what the step adds): the engine catalogue and scene
schema (the two components, two enums, four runtime types, three codes), reflect's fixture
catalogue (the codes), the CLI and serve expectations (schema hash `8ee429bddf1493f4` →
`d3d8dddea5a7226d`; the serve session's `world.hash` `f11ef83afe5a6cee` → `99a4759077e1f67f` and
the serve guide's fly hash `b85a926415d992dc` → `c8571c8108f90c68`, since the hash walks every
saved component type and two were added). No render golden changed.

## 3. Bugs found and fixed

- **The backend aborted the process on its first step with more than ~10,000 bodies.** Jolt
  takes each step's contact-constraint buffer (`maxContactConstraints × sizeof(ContactConstraint)`,
  the backend sets that to `maxBodies`) from the temp allocator, which was a fixed 10 MB block.
  The physics scene's 65,536-body cap asked for more and the process exited (127) inside
  `t07_crates_come_to_rest_when_stepped`. Reproduced in `physics_backend_tests` with a 65,536 cap
  (exit 127), fixed with Jolt's `TempAllocatorImplWithMallocFallback`; the test passes. No
  existing caller set a cap that large.
- **Scene saves grew with every registered saved type.** The writer looked up every saved type
  for every entity, present or not; this step's two types put `save_pretty` on bench10k at
  10.0008 ms against its 10 ms budget (clean rounds 9.994). It now looks up only the types some
  entity has (same bytes: `scene_tests`, the CLI goldens): clean rounds 9.318 ms afterwards.
- **An asset bug, not code:** the blockout's `SpawnEnemyB` stood 0.1 m above the ramp (y 0.6
  where the ramp is 0.5); the spawn ray found it. `blockout_room.py` now places it at 0.5, the
  GLB is regenerated (Blender 5.2.2, rebuild byte for byte), and step 1's contract says so.

## 4. Spec corrections and rulings

- **Where the physics system lives:** the plan put registration in `world.cpp`. Building a body
  needs the host's asset resolver (meshes), as instantiate does, so it is `installPhysics(world,
  assets)` in `ohao_ecs`, called by the hosts next to `installLevels`. `ohao_ecs` now links
  `ohao_physics`, which links only core, glm, nlohmann_json and Jolt: no cycle, and the ECS
  system sources still include no `reflect/` or `api/` header (`reflect_tests` t13).
- **Names:** the plan's `physics::PhysicsBackend` is `physics::backend::IPhysicsBackend`.
- **Runtime helpers** `ColliderPart`, `CollisionMesh`, `PhysicsBody`, `PhysicsMoved` are new;
  `PhysicsInfo.unstepped` counts the dynamic bodies.
- **A RigidBody only from body hints:** a level node without `body`/`mass`/`fric`/`rest` gets
  none (it is static either way).
- **Sphere and capsule scale:** fitted in the mesh's frame, then the largest scale axis, as the
  spec says (the radius bounds the mesh only for uniform scale).
- **Shape caching** per `(mesh, shape, scale)` is not built; the resolver caches meshes. Left
  for step 6, whose kit2k budgets will measure it.
- **The backend's body cap** is 65,536, single-threaded, created lazily.
- **level_bench** logs at Warn during its rounds (the per-mesh Info lines would time the console).

## 5. Results

| Gate | Result |
|---|---|
| `tools/check.py --all` | 42/46 on the final run: every functional suite passed; the four benchmarks (`scene_bench`, `serve_bench`, `level_bench`, `render_bench`) noisy |
| `level_physics_tests` | 14/14 |
| `physics_backend_tests` | 61/61 (2 new) |
| `ecs_tests` | 37/37 |
| `scene_tests` | 84/84 |
| `reflect_tests` | 42/42 |
| `engine_tests` | 85/85 |
| `api_tests` / `rpc_tests` | 44/44 / 18/18 |
| `check_cli` / `check_serve` | 25/25 (CPU) / 171/171 |
| render goldens | 25/25, every one unchanged (`cli_blockout_room_deferred` max_abs 0: collision changes no image) |
| `check_docs` / `check_live` / `e2e_mcp` | 74/74 / 42/42 / 25/25 |

**`blockout_load_ms`** (budget 20 ms, gated from step 4): structure, 13 meshes through a fresh
resolver, expansion, 13 bodies; 30 rounds after 5 warm-ups.
- Before any check, other processes at 21% of a core: median 9.425 ms, p95 12.146 ms (min
  8.378). That run's own correctness check expected 16 nodes where the blockout has 17, so it
  reported FAIL; the timing stands.
- In the two `check.py --all` runs: median 9.683 ms, p95 12.208 ms (other processes 302% of a
  core: explorer.exe 141%, UnrealEditor 97%), then median 9.094 ms, p95 13.626 ms (345%); both
  reported noisy.
- About 10.5 ms of headroom. Most of the time is the 13 Assimp imports of the same file (step 3's
  known gap).

**Benchmarks: noisy, pending a quiet rerun** (Frank's ruling). Other processes used 0.7–3.5
cores while they ran (explorer.exe, UnrealEditor, Grok Bot, Edge WebView). No budget was changed.
- `level_bench`: above.
- `scene_bench`: before the save fix (first gate run, not flagged noisy: 42% of a core, 3 of 50
  rounds contended) every budget passed but `save_pretty`, 10.0008 ms against 10 (clean rounds
  9.994); the fix of section 3 took clean rounds to 9.318 ms (a noisy standalone run) and 9.368
  ms (the final gate run, noisy: 34 of 50 rounds contended). That final run also put
  `apply_100_field_set` at 0.210 ms against 0.2 (clean rounds 0.215, 16 of them); the same apply
  code measured 0.155 ms in the first run's clean rounds, and nothing in this step runs during an
  apply (no physics or level system is installed in the bench).
- `serve_bench`: noisy in both gate runs (calibration up to 1.51x its reference, 1.35–1.48 cores
  of other processes); every measurement was inside its budget.
- `render_bench`: `cornell_720p lighting` is 0.368–0.372 ms against its 0.32 ms budget in four
  runs (one not flagged noisy, three flagged noisy on CPU or GPU calibration). This step
  changes nothing under `ohao/render`, `ohao/gpu` or `shaders/`, and render goldens are
  unchanged. The last recorded lighting medians were 0.28–0.295 ms, before the 2026-10-26 light
  mesh and area light changes rewrote the lighting shader, which recorded none. So this is a
  standing miss for Frank to rule on, not this step's regression and not noise. The budget is
  unchanged. Every other render budget passed in the clean run.

**Test 7, measured:** the spawns hit at 0.000, 0.300 and 0.500 (their authored heights, ±1 mm);
the ramp at y 0.300 with normal `normalize(0, 1, 0.2)`; the crates at y 0.3000 and 0.8000 after
120 steps, x and z unmoved; the blockout builds 11 static bodies (10 boxes, the ramp a mesh) and
2 dynamic (boxes).

**Look shots** in `C:\Users\djmax\ohao_refs\look_shots\sp5_step4\` (each pixel one ray against the
physics scene, nothing drawn from meshes; written by `level_physics_tests --look`):
- `collision_top.png`: straight down over the room, 2 cm apart, each body tinted, brighter when
  higher: the walls, the lintel over the doorway, step, platform, the ramp's gradient, the
  pillar, the crate stack.
- `collision_side.png`: from inside the east wall looking west: the ramp's slope, the stacked
  crates, the pillar, the west wall behind.
- `crates.txt`: the blockout's counts, its build time (13.7 ms there, cold, logging on) and the
  crates before and after 120 steps.

## 6. Known gaps

- **Not stepped:** dynamic bodies never move in a world (sub-project 6 steps them and syncs
  transforms back).
- **Not on the API:** `physics.raycast` and `physics.info` are step 5's; warnings are only on
  `PhysicsScene::warnings()` and the log until then.
- **Shapes are not cached** per `(mesh, shape, scale)`, and `loadMesh` still imports the whole
  file per mesh. Step 6's kit2k budgets measure both.
- **Convex hulls over 256 points** are simplified by Jolt and not yet counted in `info()`.
- **Kinematic bodies** are built (`Motion::Kinematic`) and untested beyond the catalogue.
- **The body cap** (65,536) is fixed; a body past it fails to build and is logged by the backend.
- **Benchmarks** need a quiet-machine rerun (`python tools/check.py --labels bench`).

## 7. How to run

```
build/Release/level_physics_tests.exe
build/Release/level_physics_tests.exe --look build/look
build/Release/physics_backend_tests.exe
build/Release/level_bench.exe --check tests/bench/level_budgets.json
python tests/level/make_physics_fixture.py
```
