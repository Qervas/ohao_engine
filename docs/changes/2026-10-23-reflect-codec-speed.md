# Sub-project 3, build step 9 (start): scene codec speed rewrite

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` ("Budgets (gated)", step 9 note; "Loading and validation", step 9 revision; test 21; "Open decisions", decision 1; "Decisions record", decision 3)
**Status:** Done. Frank's ruling on open decision 1 (2026-10-23): option 3. The save, validate and load budgets pass on the full bench10k. The rest of step 9 is still to do (section 8).

## 1. Summary

Save and validate on the full 10,002-entity bench10k were over their 10 ms budgets after step 8. Load was within its 20 ms budget but close. Frank ruled option 3: keep the 10 ms budgets, change no budget, stay single-threaded, and rewrite the reader and the writer. The output is byte-identical to before.

The rewrite has three parts:
- **Save** uses per-type writers for the authoring components, laid out in one pass.
- **Floats** are formatted with Schubfach, checked against `std::to_chars`.
- **Validate and load** stream clean files straight from the text. Anything else goes to the tree decoder, which reports as before.

## 2. Profiling

Profiling on bench10k found where the time went:
- The parse tree was 6.3 ms of validate's 14.9 ms.
- Save's cost was the generic reflection walk and the per-token writer, not the layout.
- A fresh 5.1 MB result string costs about 1.5 ms of page faults on its own.
- Random floats through MSVC `to_chars` cost about 25 to 28 ns each.
- Validate's Transform floats cost about 39 ns each when scanned twice.

## 3. Save (`ohao/ecs/scene_writers.hpp`, `ohao/reflect/json_layout.hpp`)

- Each authoring component has a hand-written writer, `writeFields` for `Name`, `Transform`, `Parent`, `Light`, `MeshSource`, `MaterialOverride`, `Camera` and `Environment`. Keys are literals and there is no reflection walk. A type without one takes the generic writer, laid out the same way.
- The writers fill a per-entity node list, `reflect::LayoutTree`. Each container's one-line length is known before emission, so the pretty layout is decided first and each byte is written once into a warm buffer. The layout rule is `JsonWriter`'s, exactly.
- Arrays of numbers (`vec3`, `quat`) are one node. Components are reached through their storage, resolved once per save rather than once per entity. `ComponentOps` now carries `writeFields`, `pool` and `tryGetIn` for this.
- `saveSceneGeneric` keeps the old path as the reference. Test 21 compares the two byte for byte.

## 4. Floats (`ohao/reflect/json_writer.hpp`)

- `formatFloat32` uses `shortestDecimal` (Schubfach: one 64x32-bit multiply per bound, table generated with exact arithmetic) and `writeShortestOf`, which chooses fixed or scientific the way `to_chars` does.
- A finding: for an integer of 2^24 and up in fixed notation, MSVC's `to_chars` prints the exact integer, not the shortest digits padded with zeros (`50331648`, not `50331650`). The writer does the same.
- A per-thread memo of 512 recent values returns a repeated value's text. It memoizes the same function, so the output does not change.
- All 4,278,190,080 finite floats were checked against `std::to_chars`: `reflect_tests --exhaustive-floats`, 0 mismatches, 417 s. Each normal test run checks 32M sampled floats (`writer_schubfach_matches_to_chars`).

## 5. Validate and load (`ohao/reflect/json_cursor.hpp`, `ohao/reflect/decode.hpp`, `ohao/ecs/scene_io.cpp`)

- `reflect::JsonCursor` is a pull reader over the text. It accepts exactly the tree parser's grammar and decodes escapes the same way. It does not report: the first thing it does not expect makes it fail.
- `readStructFast` and the typed readers (`FastDecode`, with `parseIntToken`, `parseHex16` and `findField`) decode straight into the same values. `numberAsFloat` reads a number once, when its value is needed. `numberFits` only checks that an unused number fits a float.
- `decodeFast` (`ohao/ecs/scene_io.cpp`) takes a document only when the tree decoder would accept it with no error, warning or upgrade. Anything else (an error, an unknown component, an older version, members out of canonical order) makes it give up, and the tree decoder decodes the same text and reports. Diagnostics do not change. Only clean files get faster.
- `validateSceneReference` and `loadSceneReference` always take the tree path. They are what the tests compare against. `decodeCounters()` reports how many decodes streamed and how many took the tree.

## 6. Results

Release build, single thread, this machine. bench10k is 10,002 entities, 5.13 MB pretty. Three runs after the rewrite, medians in ms.

| Operation (bench10k) | Step 8 | Step 9 | Budget | Result |
|---|---|---|---|---|
| `saveScene` pretty | 14.3 to 14.5 | 8.98 to 9.18 | 10 | ok |
| `scene.validate` | 14.0 to 14.9 | 8.24 to 8.46 | 10 | ok |
| `loadScene`, into an empty world | 18.7 to 19.3 | 12.8 to 13.4 | 20 | ok |
| `takeSnapshot`, `restoreSnapshot` (three), `world.query` | within | within | as spec | ok |
| `world.apply`, batch of 100 `field.set` | 0.17 to 0.21 | 0.195 to 0.217 | 0.2 | at its line |

- The apply path was not touched by this work. It is at its line, as before.
- bench100k scaling ratios are all within 12x except `restore_empty`, at x12.3 to x14.6. Restore was not touched by this work. The ratio is reported here, and the check is unchanged.

## 7. Tests

- `scene_tests` 60/60. Test 21 (`t21_*`, five tests):
  - every authoring component has a writer;
  - per-type writers equal the generic writer, byte for byte, on random worlds, in all four renderings, with the edge cases of the spec's test 21;
  - clean files stream (per `decodeCounters`), and load and re-save identically; every file of the checked-in corpus gets the same report from both decoders;
  - numbers are judged identically by both decoders, including every edge token;
  - 3,000 edited files get the same verdict and world from both decoders, 433 of them still valid.
- `reflect_tests` 31/31, including `writer_schubfach_matches_to_chars`.
- `api_tests` 15/15. `ecs_tests` 26/26. `engine_tests` 58/58. `runtime_tests` 21/21. `force_generator_tests` 54/54. `physics_backend_tests` all pass.
- `check_scene_schema.py` 14/14. `scene_view_gpu_test` 7/7. `instantiate_gpu_test` 5/5.

## 8. Tools and known gaps

- `scene_bench` writes `build/scene_bench.json` by default and creates the directory. `--out` overrides it. `/scene_bench.json` is git-ignored (reviewer's request).
- `scene_bench --breakdown` now times each fast path next to its reference: tree decode, the generic writer, and `std::to_chars`.
- `restore_empty` is over its 12x scaling limit (x12.3 to x14.6). It is not covered by the codec ruling. It is reported, not loosened.
- The untracked scratch file `examples/model_viewer_old.cpp` from step 8 is still present. It could not be deleted in this session and is not committed. Frank should delete it.
- Next: the rest of step 9 (`ohao` CLI, checked-in scenes, scene goldens, CLI goldens, restore-render test).
