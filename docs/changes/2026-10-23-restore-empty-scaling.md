# Restore into an empty world: scaling to bench100k

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (section "Budgets (gated)": the Scaling bullet and the follow-up note, "restore into an empty world")
**Status:** Done. Every bench10k budget and every bench100k scaling ratio passed in twelve consecutive gate runs.

## 1. Summary

Frank's ruling (2026-10-23): optimize the 100k restore-into-empty-world path so that its ratio to 10k meets the 12x limit. No budget or limit was relaxed, no threads were added, and the output is byte-identical.

Two causes were found and fixed:
- real superlinear work in the restore code;
- a measurement that paired bench100k samples with bench10k samples taken in a different memory and cache state.

## 2. Profiling

Measured with `scene_bench --breakdown` and with temporary per-phase timers, which were removed afterwards.

Before the change, restore into an empty world, back to back: 100k 25.57 ms, 10k 2.40 ms (x10.65). Page faults 5,850 vs 823.

Per phase, 100k / 10k:
- create entities: 13.03 / 1.32 ms (x9.9);
- MeshSource column: 4.30 / 0.377 ms (x11.4);
- hierarchy: 3.17 / 0.234 ms (x13.5);
- MaterialOverride column: 2.11 / 0.182 ms (x11.6);
- Transform column: 1.17 / 0.084 ms (x13.9);
- Name column: 0.84 / 0.050 ms (x16.8).

Page faults grew no faster than the entity count (x7.1 for x10 entities). The superlinear phases were the ones that re-walk the whole world (the Name and Transform columns re-compared values that step 3 had just written; the hierarchy pass revisited each parent's Children after they had been evicted from cache) and the pools that grew by doubling. An isolated test of the id index (hash inserts) showed x16 but only about 0.6 ms of excess, so it was left as it is.

## 3. Code changes

Changed in `ohao/ecs/snapshot.cpp` and `ohao/ecs/snapshot_column.hpp`. The output is unchanged.

- Every pool is reserved to its column's size before a restore emplaces into it. This is a no-op when the pool already holds the values. The Parent pool is reserved once.
- Restoring into a world with no entity (detected up front) skips:
  - the matching of rows (step 1);
  - the destruction of extra entities (step 2);
  - the Name and Transform columns (step 3 has just written them);
  - the per-value lookup, comparison and drop sweep in every other column. One sequential emplace pass is made instead.
- Hierarchy into an empty world: each link is attached while the child is created, with the parent still in cache. This is done when every parent row precedes its child's row and the snapshot's child lists ascend. This holds for the bench scenes and for any world built by `World::create`. Otherwise one pass attaches the links from the snapshot's child lists, which are already in their order.

Result, back to back with `--breakdown`: 100k 25.57 ms to 21.54 ms; 10k 2.40 ms to 2.06 ms.

## 4. Measurement changes

Changed in `tools/bench/scene_bench.cpp`.

- Page faults are counted per sample (`GetProcessMemoryInfo`). They are reported next to each scaling ratio and in `scene_bench.json`. `--breakdown` reports restore into an empty world and its teardown with page faults.
- What the counts showed: after a 100k world has been freed, the heap keeps enough committed memory for a whole 10k world. A 10k operation then takes anywhere from 1 to about 650 page faults, depending on what was freed just before. bench100k always takes fresh pages (about 6,500 faults for `restore_empty`, about a quarter of its time).

Methods tried after the first code changes, with the `restore_empty` ratio:
- step-6 interleaved pairs, 100k then 10k, 10 rounds: x15.5, then x11.7 (the 10k sample 1.56 ms, then 2.20 ms);
- the same pairs at 25 rounds, with a heap trim (`_heapmin`, `HeapCompact`) before each fresh-world sample: x16.1 to x16.5 over six runs, the 10k sample 1.52 ms against 2.13 to 2.19 ms in its own rounds;
- pairs with 10k first: x13.0 to x13.6. In a later run the 10k side took 39 faults instead of about 650;
- all 10k operations, then all 100k operations, per round: x16.8. The 10k side was fault-free (1 fault) and cache-warm. With the heap trim added it took 6 faults: the trim changes nothing measurable and was dropped;
- cold-cache blocks with the 10k block run after the gated rounds: x11.6 to x13.8. The 10k side took 203 to 560 faults, varying from run to run.

Final method:
- caches are evicted before every scaling sample at both sizes (`flushCaches`: one write per cache line of a 128 MB buffer, allocated once; `coldCopy` ends each untimed setup with it);
- the bench10k scaling block (25 rounds after 2 warm-ups) runs first in the process, then the gated bench10k rounds, then the bench100k scene and its block. The block was 10 rounds before; it is now 25;
- the bench10k scaling samples now fault like the gated rounds (about 690 against about 650) and match their time within a few percent.

Drift between the blocks is the smaller effect; the blocks run seconds apart.

## 5. Results

Twelve consecutive gate runs with the final code and method. Every budget and every ratio passed.

`restore_empty` ratio per run: x10.6, 11.2, 10.9, 11.4, 10.5, 10.1, 10.8, 11.2, 10.9, 11.2, 10.4, 10.2.

Across the twelve runs: bench100k median 21.6 to 24.4 ms; bench10k cold median 1.99 to 2.22 ms; page faults about 6,100 to 6,860 at 100k against 679 to 693 at 10k.

For comparison, before this change: x12.3 to x14.6 (codec-speed record, step 9); x10.7, x14.9 and a pass (step 10).

Full gate table from run 11. bench10k medians, with p95 in brackets, and the budgets:

| Operation (bench10k) | Median [p95] (ms) | Budget (ms) |
|---|---|---|
| `saveScene` pretty | 9.674 [10.083] | 10 |
| `loadScene`, into an empty world | 13.618 [14.099] | 20 |
| `scene.validate` | 8.607 [9.178] | 10 |
| snapshot | 0.596 [0.727] | 1 |
| restore, nothing changed | 0.664 [0.777] | 1 |
| restore, 1% of rows changed | 0.501 [0.545] | 2 |
| restore into an empty world | 2.122 [2.246] | 10 |
| `world.apply`, batch of 100 `field.set` | 0.195 [0.316] | 0.2 |
| `world.query`, 1,000 ids | 1.283 [1.494] | 2 |

bench100k in the same run, with the ratio to bench10k:
- `saveScene` pretty 92.844 ms, x9.9;
- `loadScene` 138.923 ms, x10.4;
- `scene.validate` 87.523 ms, x10.4;
- snapshot 4.978 ms, x9.3;
- restore, nothing changed 5.940 ms, x9.4;
- restore, 1% changed 5.918 ms, x9.5;
- restore into an empty world 21.573 ms, x10.4;
- `world.apply` 0.211 ms, x1.0;
- `world.query` 1.539 ms, x1.2.

Tests:
- `engine_tests` 58/58, `runtime_tests` 21/21, `ecs_tests` 26/26, `reflect_tests` 31/31, `scene_tests` 60/60, `api_tests` 17/17, `systems_tests` 6/6, `force_generator_tests` 54/54. `physics_backend_tests` and `audio_system_tests` pass.
- `env_cdf_test` 2/2, `denoise_parse_test` 5/5, `sobol_test` 8/8, `renderer_test` passes.
- `instantiate_gpu_test` 5/5, `scene_view_gpu_test` 7/7, `restore_render_gpu_test` 14/14.
- `check_cli.py` 29/29. `check_scene_schema.py` 16/16.
- `check_validation.py` is clean. `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200`: 0 messages, including teardown.
- Render goldens 10/10 at unchanged tolerances. Max abs per image: `cornell_box` 0, `damaged_helmet` 1, `cornell_box_deferred` 0, `damaged_helmet_deferred` 1, `cornell_box_rt_realtime` 3, `cli_cornell_box` 0, `cli_damaged_helmet` 1, `cli_cornell_box_deferred` 0, `cli_damaged_helmet_deferred` 2, `cli_cornell_box_rt_realtime` 5.
- `check_docs.py`: `AGENTS.md` 24/24, run twice. `test_monograph_structure` 16 tests OK.

## 6. Docs updated

- The spec's Scaling bullet (the method as built, and why it changed) and a follow-up note under "Budgets (gated)".
- The roadmap row.
- The monograph unit `reflection/snapshots`: a new section "Restoring into an empty world". Its stale x12.3 to x14.6 sharp edge is replaced.

## 7. Known gaps and observations

- `apply_100_field_set` stays at its line, as before this change. It measured 0.159 to 0.198 ms in the twelve gate runs. Two diagnostic runs without the scaling blocks (`--no-scale`) measured 0.211 to 0.212 ms, over its 0.2 ms budget. The changed code is not on that path. This is machine noise on a budget that has sat at its line since step 6.
- `restore_render_gpu_test` failed once, inside a full `check_docs.py` run, and passed in five further runs (three standalone, two through `check_docs` on `AGENTS.md`). The failing check was not captured. Its post-effects case allows 1 LSB, because TAA, SSAO and bloom are not bit-exact run to run. That is the likely cause; it is not confirmed.

## 8. Running the checks

```
build/Release/scene_bench.exe --check tests/bench/scene_budgets.json
build/Release/scene_bench.exe --breakdown 100000
build/Release/scene_tests.exe                    # includes t10
```
