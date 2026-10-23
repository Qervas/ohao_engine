# Sub-project 3, build step 2: stable entity ids and engine registrations

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 2)
**Status:** Done; committed in `e3ae9ef` (local, not pushed).

## 1. Summary

Step 2 gives every entity a stable 64-bit `EntityId` and indexes entities by it. It registers every engine component with `ohao_reflect`, authoring and runtime, so the catalogue describes all of them. It adds the `mat4` kind to the codec, a CPU-only test target `scene_tests` in `tests/scene`, and the pinned ankerl::unordered_dense v4.4.0 dependency for the id index.

`ohao_ecs` now links `ohao_reflect` and `ohao_unordered_dense`. Per-frame systems do not include `ecs/scene_types.hpp`; its header says reflection serves load, save, validation, commands and tools only.

## 2. Stable entity ids

### 2.1 The `EntityId` component

`EntityId { uint64_t value; }` is in `ohao/ecs/components.hpp`. `World::create` and `World::createWithId` assign it. 0 is invalid. Nothing else should emplace or edit it.

It is not a registered component. It is the entity's key, written as `"id"` on each entity in a scene file, so it has no component block of its own. `t01_authoring_and_runtime_split` checks that `find("EntityId")` returns `nullptr`.

### 2.2 The generator

Generated ids are `splitmix64(seed + counter++)`, in `ohao/ecs/world.cpp`, skipping 0 and any id already in use.

- splitmix64 is bijective on 64 bits. For a fixed seed, `seed + 0`, `seed + 1`, and so on never repeat before 2^64 creations.
- The counter never moves back, so an id freed by `destroy` is not generated again. `t05_destroyed_ids_are_not_regenerated` checks this. A freed id can still be claimed explicitly.
- The seed comes from `std::random_device` (two 32-bit draws) unless `seedIds(uint64_t)` is called. `seedIds` also resets the counter to 0.
- `IdGenState { seed, counter }` is the whole generator state. `idGenState()` reads it and `setIdGenState()` restores it. Restoring a state repeats the ids that follow it (`t05_generator_state_repeats_future_ids`). Snapshots will store this pair (step 6).

### 2.3 Explicit ids

`World::createWithId(id, name, local)` uses the given id. It returns `entt::null` and creates nothing when `id` is 0 or already in use. Generated ids skip ids in use, so explicit and generated ids never collide. `t05_explicit_ids_are_accepted_and_duplicates_rejected` tests this by claiming the id a twin world is about to generate.

### 2.4 The id index

`World::m_byId` is an `ankerl::unordered_dense::map<uint64_t, entt::entity>`. The API is:

- `findById(id)` returns the entity, or `entt::null`.
- `idOf(e)` returns the id, or 0 when `e` is invalid or has no `EntityId`.
- `idCount()` returns the number of indexed entities.

The index is kept by three EnTT hooks on `EntityId`, connected in the `World` constructor:

- `on_construct` inserts the id. If the id is already present on another entity, it logs to `std::cerr`.
- `on_destroy` erases the entry, but only when it points at the entity being destroyed.
- `on_update` re-indexes an edited id. It finds the old entry by a linear scan, since the event does not carry the old value. Edits are not expected, so the scan is not a cost on any normal path.

The hooks touch only `m_byId`, never a pool. Because they sit on the registry signals, the index holds through `World::destroy`, `registry().destroy(e)` and `registry().clear()`. `t05_find_by_id_follows_every_destroy_path` checks all three, and `t05_edited_id_is_reindexed` checks the edit path.

The dependency is `external/cmake/unordered_dense.cmake`. It fetches v4.4.0 with FetchContent and exposes the `ohao_unordered_dense` interface target. The version is pinned and changes only deliberately.

## 3. Component registrations

`registerSceneTypes(reflect::TypeRegistry&)` in `ohao/ecs/scene_types.cpp` registers every ECS component. The order is fixed, and it is the order of the catalogue and of component blocks in scene files. Each field has a doc. Each numeric field has a unit or `.unitless()`, as lint requires.

### 3.1 Saved components

These are authoring components. They are `.component()` types and are written to scene files.

- **Name**: `value` (string). Display name; not unique.
- **Transform**: `position` (vec3, m); `rotation` (quat, unitless, written `[x, y, z, w]`); `scale` (vec3, unitless, a negative value mirrors).
- **Parent**: `entity` (entity reference). Null makes the entity a root. Cycles are rejected at the World API.
- **Light**:
  - `type` (LightType enum);
  - `color` (vec3, linear-rgb, min 0 per component);
  - `intensity` (float, unitless, min 0, UI range 0 to 100);
  - `range` (float, m, min 0);
  - `radius` (float, m, min 0);
  - `innerConeDeg` and `outerConeDeg` (float, deg, angle, 0 to 180);
  - `areaEdge1` and `areaEdge2` (vec3, m, no limits).

**LightType** is a registered enum with four values: `Point`, `Directional`, `Spot` and `AreaRect`. Its docs state that position and direction come from the world transform, and that the direction is the entity's -Z axis.

### 3.2 Runtime components (catalogue only)

These are `.runtime()` types. They appear in the engine catalogue with `serialized: false` and are not in the scene schema. Most are `.component()` types too. **MeshHandle** and **MaterialHandle** are plain runtime types, used as field types of `MeshRenderer`.

- **MeshHandle** and **MaterialHandle**: `index` and `generation`, both unitless.
- **Children**: `entities` (entity vector). Derived from `Parent`.
- **WorldTransform**: `world` and `prevWorld` (mat4, unitless, column-major).
- **MeshRenderer**: `mesh` (MeshHandle), `materials` (MaterialHandle vector), `visible` (bool).
- **RenderSlot**: `slot` (unitless).
- **EmissiveProxyLight**: `localCenter` (vec3, m), `radius` (m), `color` (linear-rgb), `intensity` (unitless).
- **GiMaterialLegacy**: `packed` (vec4, unitless). Marked LEGACY in its doc.
- **CreationOrder**: `seq`. Never recycled.
- **Tags** `TransformDirty`, `RenderDirty` and `MovedLastFrame`: empty types with a doc and no fields.

### 3.3 Entity references and the include rule

`scene_types.hpp` specializes `reflect::ValueTraits<entt::entity>`:

- `isEntityRef = true`, so `entt::entity` fields are `EntityRef`;
- `toRaw(e)` returns the integral value;
- `fromRaw(raw)` and `null()` are provided for reading (the reader is step 3).

Reflect itself never includes EnTT. Any file that reflects ECS components must include `ecs/scene_types.hpp`, not only the registry. The specialization must be visible at every instantiation; without it, an `entt::entity` member cannot be registered.

### 3.4 WorldEntityIds

`WorldEntityIds` implements `reflect::EntityIdMapper` over a `World`. It is set on `JsonWriter::entityIds`. A live reference writes the target's id as 16 hex digits. A dead or id-less target writes `null`. `t05_entity_refs_write_ids_and_dangling_null` checks a live reference, a dead one, and a child vector.

## 4. Reflect change: the `mat4` kind

`Kind` gains `Mat4`, for `glm::mat4` in `ohao/reflect/codec.hpp`. A matrix is written as a 16-element array of floats in column-major order: for each column, its four rows. Equality is bit-exact, like the other float kinds. `WorldTransform` is the first user.

The kind is wired through `type_info.hpp` (the enum value), `registry.cpp` (`typeName` returns `"mat4"`, and it takes a unit) and `schema.cpp` (a 16-number array in the JSON Schema, without limits). Step 1's 17 kinds are now 18.

## 5. Tests

`tests/scene/scene_tests.cpp` is a new CPU-only executable, `scene_tests`. It links `ohao_ecs` and `pfr_test_only` and gets the `OHAO_SOURCE_DIR` define. It has 13 tests, all passing:

- **t05, ids (8):** non-zero unique ids, index and count; seeded reproducibility; generator state restore; `findById` through every destroy path; explicit ids and duplicate rejection; no regeneration after destroy; edited-id re-index; entity references as ids and nulls.
- **t01, completeness and registrations (3):** every member of every registered component is registered or skipped, checked with Boost.PFR (test-only); lint is clean; the authoring and runtime split is correct and the schema excludes runtime types.
- **t12, goldens (1):** the engine catalogue and the scene JSON Schema match `tests/scene/golden/engine_catalogue.json` and `engine_ohscene.schema.json` byte for byte.
- **Benchmark (1, informational):** `bench_write_authoring_components`. Section 7 has the numbers.

The goldens are regenerated only after a deliberate change. `.gitattributes` pins `tests/scene/golden/**` to LF, the same as the reflect goldens.

Build and run, from the repository root (Visual Studio generator, binaries in `build/Release/`):

```
/c/Users/djmax/scoop/shims/cmake --build build --config Release --target scene_tests
build/Release/scene_tests.exe
build/Release/scene_tests.exe --update-golden
```

`scene_tests.exe` prints a JSON summary as its last line and exits 0 on success. `--update-golden` rewrites both golden files from the engine registry.

## 6. Verification

- `reflect_tests` 22/22.
- `check_scene_schema.py` 14/14.
- `scene_tests` 13/13 (section 5).
- `ecs_tests` 26/26.
- `engine_tests` 58/58.
- `runtime_tests` 21/21.
- `renderer_test`: all pass.
- physics 59/59.
- Force generators 54/54.
- Goldens 5/5 PASS, with unchanged tolerances.
- `check_validation.py` is clean.

Not run: the windowed `OHAO_VALIDATION=1 ohao_game` run. The session's tool permissions refused environment-prefixed commands, so it was not executed in this step.

## 7. Performance

Informational. Release build on Frank's laptop. Not gated in this step.

The benchmark writes 10k entities with `Name` and `Transform` on each, random rotations, a `Parent` on a third of them, and 100 `Light` components. Across two runs on the same build:

- write: 3.7 to 5.5 ms (2.0 MB compact);
- pretty print: 4.8 to 6.8 ms (2.7 MB);
- total: 8.5 to 12.3 ms.

The laptop's timings vary by about ±30% between runs.

The spec's `bench10k` scene also carries `MeshSource` and `MaterialOverride` on 9,900 entities, which are not registered yet (step 7). Extrapolating from the measured sizes gives about 16 to 23 ms against the 10 ms save budget. The extrapolation is an estimate, not a measurement.

Pretty printing is about 55% of the cost. Step 6 measures and gates this. The candidates are a single-pass pretty writer and faster float formatting.

## 8. Spec adjustments made in this step

- The `mat4` kind is added to the codec and the catalogue.
- `createWithId`, `IdGenState` with `idGenState`/`setIdGenState`, and the `on_update` hook are documented.
- `EntityId` is not a registered component; it is the entity key.
- The test targets are `tests/reflect` → `reflect_tests` and `tests/scene` → `scene_tests`.
- Step 2 is marked done, with runtime components registered too (`.runtime()`), so the catalogue covers every component.

## 9. Deferred to later steps

- Decoding and reading, the simdjson reader, the two-phase load, and the scene save/load envelope: step 3.
- Snapshot use of `IdGenState`: step 6.
- Registrations for `MeshSource`, `MaterialOverride`, `Camera` and `Environment`: steps 7 and 8.
