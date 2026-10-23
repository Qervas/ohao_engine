# Sub-project 3, build step 6: snapshots, restore, scene_bench and the budget gate

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 6; "Snapshots", "Performance", "Budgets")
**Status:** Done; committed in `a92c8f4`.

## 1. Summary

Step 6 adds in-memory columnar snapshots and restore (`ohao/ecs/snapshot`), exposed as `world.snapshot` and `world.restore` in the API. Restore diffs the live world against the snapshot and writes only what differs. It adds `scene_bench` (`tools/bench`), which times the gated operations and checks them against `tests/bench/scene_budgets.json`. Every gated budget and the bench100k scaling ratio pass on bench10k.

Most of the step is performance work. The first measurements failed the save, load and validate budgets. Profiling pointed at the JSON reader, per-entity allocations in decode, float conversion during validation and the pretty writer. Those were fixed before the gate passed. The budgets themselves are the ones in the spec. Save and validate have the least headroom (section 5).

## 2. Snapshots (`ohao/ecs/snapshot.hpp`, `snapshot.cpp`, `snapshot_column.hpp`)

### 2.1 What is captured

- The entity table, in creation order: `EntityId`, `CreationOrder`, the parent row, and the child rows in `Children` order. Sibling order comes back exactly.
- One column per saved component type. `Parent` is left out of the columns because the parent row covers it. `PreservedComponents` is captured too. A column holds the values in EnTT packed order, and each value's entity row.
- The id generator `(seed, counter)`, the next creation sequence, and the frame counter passed in by the caller.

### 2.2 What is not captured

- Derived runtime components (`Children`, `WorldTransform`, render slots and GPU state). Their owners rebuild them from the restored components through the same signals an edit fires. `setParent` keeps `Children` current, and a changed `Transform` is tagged dirty. Simulation state that must survive a restore has to live in a saved component.
- Entities without an `EntityId`. Capture walks the `EntityId` storage only, and restore destroys only extra entities that have an `EntityId`. Other entities are left alone.

### 2.3 Capture

- Rows are in creation order. `CreationOrder` is read per packed position. If the packed order is not already sorted, the rows are stable-sorted by sequence. For a world built by `World::create` the packed order is creation order, so the reads are sequential.
- Trivially copyable components (`Transform`, `Light`) are copied with one `memcpy` per EnTT storage page. Non-trivial components (`Name`, `PreservedComponents`) are copied value by value in packed order.
- The child lists are stored in compressed form (`childBegin` and `children`). The snapshot also records whether every child list is in ascending row order, which lets restore skip a sort check.
- Recapture reuses memory. A column keeps its value vector when its type is unchanged, and the thread-local working vectors stay warm. The snapshot is move-only, because its columns own buffers that a recapture reuses. `world.snapshot` over an existing name recaptures into that snapshot. The unit test checks that the buffer pointer is unchanged.

### 2.4 Restore

Restore runs in this order:

1. **Match live entities.** Snapshot row `r` is matched first by position: if `EntityId` storage slot `r` holds the same id, that is the entity. Otherwise the id index is used. A matched entity gets its saved `CreationOrder`. A changed sequence counts as a change.
2. **Destroy extras.** Every entity with an `EntityId` that no row matched is destroyed with `registry().destroy`. That call detaches the extra entity's children instead of destroying them. Children that belong to the snapshot are reattached in step 5.
3. **Create missing entities.** Each missing row is created with `World::createWithId`, using its saved id, `Name` and `Transform` (the defaults if absent), and its saved `CreationOrder`. Rows are created in order. `World::reserve` is called first. Creations in this step are not counted as changes.
4. **Write component values, column by column.** A listed value is emplaced if the entity lacks the component. If the entity has it, the value is compared bit-exact, per registered field, with the save's equality test. It is replaced only when it differs. Managed entities that the snapshot does not list for a component have that component removed.
5. **Hierarchy.** Each entity whose current parent differs from its snapshot parent is detached first. Then all of them are attached with `World::attach`. Detaching first means no intermediate state has a cycle, and the attached links are all snapshot links, so no ancestor walk is needed. Then each `Children` list is put back in snapshot order. When this restore attached every link itself and the snapshot's child lists are ascending, this check is skipped.
6. **Id generator and sequence.** The id generator state and the next creation sequence are set to the saved values, so new ids repeat.

An unchanged restore writes no component and fires no signal. The test counts signals. A restore of an unchanged world therefore uploads no instance bytes to the GPU (the GPU check itself is step 9, see section 8).

### 2.5 RestoreStats

- `created`: entities recreated by step 3.
- `destroyed`: extra entities destroyed by step 2.
- `changed`: component writes and component removals on entities that existed before the restore, reparents on those entities, and `CreationOrder` corrections on those entities. Recreated entities do not count their own writes.

`restoreSnapshot` never fails. The snapshot must come from a world with the same type registrations.

### 2.6 API and errors

- `world.snapshot {name}` returns `{name, entities, bytes}`. Snapshots are kept in the `Session`, by name. `bytes` is `Snapshot::bytes()`, the memory the snapshot holds. An existing name is recaptured in place.
- `world.restore {name}` returns `{created, destroyed, changed, frame}`. The snapshot is kept, so restoring it again is cheap. The session frame is set back to the frame saved in the snapshot.
- An unknown name is `E_SNAPSHOT_UNKNOWN`, with the existing names as the suggestion (`oneOf`), or the hint "a name given to world.snapshot (none yet)" when there are none. A missing `name` is `E_COMMAND`.
- `errorCodes()` (`ohao/reflect/decode.cpp`) now has 26 entries: 25 `E_` codes and the one warning code `W_UNKNOWN_COMPONENT`. `E_SNAPSHOT_UNKNOWN` is the new one.

## 3. scene_bench (`tools/bench/scene_bench.cpp`)

### 3.1 Options

- `scene_bench [--check budgets.json] [--out file] [--no-scale] [--iters N]`.
  - `--check` reads the budgets and exits 1 when a median is over its budget or a bench100k ratio is over `maxRatio`. An unreadable budgets file exits 2.
  - `--out` sets the results file (default `scene_bench.json`).
  - `--no-scale` skips bench100k.
  - `--iters` sets the number of recorded rounds (default 50).
- `scene_bench --breakdown [N]` times the codec's phases separately: parse pretty and compact, validate, load, save in both layouts, `prettyPrint`, and float formatting. It runs 3 warm-up and 20 recorded rounds, defaults to 10,000 entities, and is not gated.
- The process raises its priority to high on Windows.

### 3.2 bench10k composition (step 6)

The generator is `buildBenchWorld` with seed 2026. It creates 10,000 entities:

- `Name` and `Transform` on every entity, with random position, rotation and scale.
- 100 lights of every type (1% of the entities).
- A hierarchy of depth up to 8. About 60% of the entities get a parent among the 64 created just before them.
- 3.2 MB pretty.

`MeshSource`, `MaterialOverride`, `Camera` and `Environment` are not in the scene yet. Steps 7 and 8 add them, and the budgets are re-checked then.

bench100k is the same generator with 100,000 entities. Its pretty output is 32.36 MB.

### 3.3 Method

- Operations run interleaved: one of each per round, so a slow phase of the machine lands on all of them. Each operation is timed on its own. Setup and teardown are untimed.
- bench10k runs 5 warm-up rounds, then 50 recorded rounds. The reported figures are the median and p95 of those samples.
- After timing, `verify()` runs every operation once more and checks its result. The checks are: save is deterministic; the scene validates; load reproduces the save; the snapshot holds every entity; an unchanged restore reports zero created, destroyed and changed; restoring after moving 1% of the rows reports exactly those rows and reproduces the save; restore into an empty world reproduces the save; the batch reports 100 applied; the query returns 1000 entities. A failed check fails the run.
- The 1% restore moves the position of every 100th snapshot row before each run.
- bench100k is paired with bench10k. Each bench100k operation runs in the same rounds as its bench10k copy: 2 warm-up rounds and `max(10, iters / 5)` recorded rounds (10 at the default). The scaling ratio is the bench100k median divided by the paired bench10k median from those rounds.

### 3.4 Budgets and output

- `tests/bench/scene_budgets.json` holds `budgetsMs` for the nine gated operations and `scaling` with `maxRatio` 12. The gate compares medians only.
- `scene_bench.json` records each scene's entity count, byte sizes, and each operation's median, p95, budget or ratio, plus `passed` and any `failures`.

### 3.5 Removed tests

Two informational benchmarks in `tests/scene/scene_tests.cpp` are removed: `bench_scene_save_load_10k` and `bench_write_authoring_components`. Their measurements are now in `scene_bench`.

## 4. Performance work

Each change was made after profiling showed the cost. The commit message gives the list.

- **JSON reader.** `ohao/ecs/json_parse.cpp` replaces simdjson On-Demand plus a tree builder. The reader is single-pass, strict RFC 8259, with an explicit stack and a 1024-level nesting limit. Number tokens and unescaped strings are views into the text. Escaped strings go to an arena. Whitespace is skipped 16 bytes at a time. simdjson now only validates UTF-8. Parse of bench10k pretty went from 6.8 ms to 3.6 ms. The per-value API of On-Demand was the cost.
- **Decode.** `ohao/ecs/scene_io.cpp` keeps decoded entities and values in flat tables (`DecodedEntity` is a range of values) instead of allocating per entity. Reference records and paths live in the same warm storage. A reference's JSON Pointer is built only when an error is reported (`DecodeContext::pathOf`). The cycle check uses a vector.
- **Warm working memory.** Decode state is kept per thread (`WarmMemory`), and so is the parser. Vectors over about 512 KB are returned to Windows when freed and page-faulted again on the next use. Pushing 10k reference records cost 1.1 ms that way before the change.
- **Validation without conversion.** `floatFitsQuick` (`ohao/reflect/decode.cpp`) decides from the decimal exponent whether a number in a field without limits fits a float. Tokens near either end of the float range, or with long exponents, are converted. `readField` turns conversion back on for fields with limits. The verdict matches load (section 6).
- **Pretty writer.** `JsonWriter` breaks a container as soon as it cannot fit on the line. It used to re-lay out whole subtrees at the end. The output is byte-identical to `prettyPrint`, which a random-tree test checks.
- **World.** `World::attach` links a child without the ancestor walk, for hierarchies already validated. `World::reserve` pre-sizes the id index and the storages before a bulk create. A created id takes one hash operation (`claim`), not two.
- **Snapshot and restore.** Both read and write in packed order, with no per-entity sparse or hash lookup in the common case. Working memory is thread-local, so an agent that restores every step does not allocate each time.

## 5. Results

Release build, single thread, this machine. Medians and p95 in ms, from the step-6 run. Every gated value is within its budget.

| Operation (bench10k) | Median | p95 | Budget |
|---|---|---|---|
| `saveScene` pretty | 8.288 | 9.610 | 10 |
| `loadScene`, into an empty world (teardown untimed) | 11.449 | 13.486 | 20 |
| `validateScene` | 7.618 | 10.065 | 10 |
| `takeSnapshot` | 0.339 | 0.472 | 1 |
| `restoreSnapshot`, unchanged | 0.341 | 0.484 | 1 |
| `restoreSnapshot`, 1% of rows changed | 0.265 | 0.354 | 2 |
| `restoreSnapshot`, into an empty world | 1.708 | 2.230 | 10 |
| `world.apply`, batch of 100 `field.set` | 0.117 | 0.172 | 0.2 |
| `world.query`, 1,000 ids, full components | 0.825 | 1.010 | 2 |

bench100k (32.36 MB pretty). The ratio is against the paired bench10k median from the same rounds. The limit is 12x.

| Operation (bench100k) | Median (ms) | Ratio to paired bench10k |
|---|---|---|
| save | 197.5 | x10.0 |
| load | 289.3 | x10.3 |
| validate | 181.3 | x10.3 |
| snapshot | 6.02 | x9.8 |
| restore, unchanged | 7.53 | x10.4 |
| restore, 1% | 7.06 | x10.3 |
| restore, into an empty world | 43.8 | x10.2 |
| apply, 100 `field.set` | 0.355 | x1.3 |
| query, 1,000 ids | 2.20 | x1.5 |

Points to read from these numbers:

- Against the warm, standalone bench10k medians in the first table, bench100k is 22 to 27 times slower for save, load, validate and the three restores. It is 18 times slower for snapshot, and 3.0 and 2.7 times for apply and query. The gate does not use these standalone ratios. It uses the paired ratio, which the spec documents. The standalone figures show the scene outgrowing the CPU caches (32 MB of text and a world of about 25 MB), not per-entity work growing. The paired ratios are the ones that show work per entity is near-linear.
- Save has about 17% headroom against its budget, and validate about 24%. The full ~5 MB scene of steps 7 and 8 must be re-gated then. If it misses, profile first. A budget change needs Frank's OK.

## 6. Tests

- `scene_tests` 38/38. The new tests:
  - `t10_mutations_then_restore_give_identical_saves`: moves, adds, removes, renames, reparents, destroys and raw destroys, then restores. The save is byte-identical and each `Children` list is back in order. A second restore changes nothing.
  - `t10_id_counter_and_creation_sequence_are_restored`: after a restore, new entities get the same ids and sequence.
  - `t10_unchanged_restore_writes_no_component`: an unchanged restore fires no signal on any watched type. One changed row is one replace.
  - `t10_restore_into_an_empty_world`: every entity is created, with the same `CreationOrder` and the same save.
  - `t10_restore_after_structural_changes_keeps_creation_order`: after destroys and an intruder, light order by `CreationOrder` is exact.
  - `t10_children_order_is_restored`: `Children` order comes back after reparenting.
  - `t10_recapture_reuses_memory_and_matches_a_fresh_snapshot`: recapture reuses the buffers, and restores the recaptured state.
  - `t06_json_reader_is_strict_rfc8259`: a set of valid and invalid documents (including invalid UTF-8 and lone surrogates), nesting of 1000 levels accepted and 1025 rejected, and line and column in errors.
  - `t06_json_reader_strings_and_numbers`: escapes, surrogate pairs, unescaped strings as views into the text, and number tokens kept.
  - `t06_validate_reports_what_load_reports_for_float_edges`: 21 float edge tokens. Validation and load give the same verdict on each.
- `api_tests` 13/13. New: `t10_snapshot_and_restore_through_the_api`. It follows the spec's script (snapshot, edits, steps, restore) and checks the created, destroyed and changed counts, the frame, the save, the repeated next id, `E_SNAPSHOT_UNKNOWN` with its suggestion, `E_COMMAND` for a missing name, and that a second snapshot under the same name replaces the first.
- `reflect_tests` 29/29. New: `t02_pretty_writer_matches_pretty_print_on_random_trees`. It compares the pretty writer with `prettyPrint` over 300 random trees at widths 20, 60 and 100.
- `check_scene_schema.py` 14/14; `ecs_tests` 26/26; `engine_tests` 58/58; `runtime_tests` 21/21; `force_generator_tests` 54/54; `physics_backend_tests` all passed; `renderer_test` ALL PASS.
- Render goldens 5/5 PASS. `cornell_box` and `cornell_box_deferred` match exactly. The goldens were run because `World::create` changed.
- `scene_bench --check tests/bench/scene_budgets.json` PASS.

## 7. Spec adjustments made in this step

- "Loading and validation": the reader is the single-pass reader (decision 3 revised again). `Parent` is applied through `World::attach`. The validation float shortcut is described.
- "Snapshots" (under "Performance"): rewritten to the design as built: what is and is not captured, capture order and memcpy per page, the restore steps, and `RestoreStats`.
- "Measured by scene_bench": interleaved rounds, the post-timing verification, the paired scaling method, and the high process priority. The step-6 bench10k composition is noted.
- The "Performance" and "Budgets" sections: the budgets are measured, and the headroom note is added.
- Error table: `E_SNAPSHOT_UNKNOWN` added under "API and commands".
- API table: `world.snapshot` and `world.restore` rows describe the results and the errors.
- Dependencies: simdjson now only validates UTF-8 (decisions 3 and 32, and the risks section).
- Build order: step 6 marked done.

## 8. Known gaps and deferred

- `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200` is not run. Environment-prefixed commands are denied in this session, so Frank runs it.
- The old `*.ohscene` files from step 5 are still tracked. Frank removes them with the command in the step-5 record:

  ```
  git rm -r --quiet -- '*.ohscene' tests/reflect/golden/fixture_ohscene.schema.json tests/scene/golden/engine_ohscene.schema.json
  ```

- The full ~5 MB scene of steps 7 and 8 must be re-gated. Save and validate have the least headroom (section 5).
- "Restore renders identically and uploads zero instance bytes" is a GPU check. It is test 17, step 9.
- Snapshots live in memory only. The cooked binary format is not written yet.

Next: step 7 (MeshSource, MaterialOverride, generateQuad/generateBox, instantiate, resolver).
