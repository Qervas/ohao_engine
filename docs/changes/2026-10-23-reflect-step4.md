# Sub-project 3, build step 4: versioning and migrations

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 4; "Versioning and migration")
**Status:** Done; committed in `080eb1e` (local, not pushed).

## 1. Summary

Step 4 adds per-component versions and migrations. A component type registers an upgrade for each older version it still reads. On load, the file header's `components` map decides, per type, whether a block is decoded directly or upgraded first. Upgrades run in phase 1, so a failed migration leaves the world untouched. `JsonValue` in `ohao/reflect` gives migrations a small mutable JSON value to edit. The error table gains `E_UPGRADE`, the catalogue gains `readsVersions`, and the `tests/scenes/v1` corpus is added.

## 2. JsonValue (`ohao/reflect/json_value.*`)

`JsonValue` is an owning, mutable JSON value. Object member order is kept. Numbers keep their token text, as `JsonNode` does.

### 2.1 Constructors

- `boolean(b)`, `string(s)`, `array()`, `object()`, and the default constructor (null).
- `number(float)` writes the canonical shortest form. A non-finite value becomes null.
- `number(int64_t)` writes an integer token.
- `numberToken(token)` stores the text as given. It does not check that the text is a JSON number.

### 2.2 Reads and edits

- `type()`, `isNull()`, `asBool()`, `text()`, `items()` (array elements, or object values parallel to `keys()`) and `keys()`.
- `asFloat()` returns a correctly rounded float for a number token, and nullopt for non-numbers and values that are not a finite float. `asInt()` accepts integer tokens that fit in int64.
- `find(key)` returns null for a missing key. `set(key, v)` replaces the member in place, or appends it. `erase(key)` returns false for a missing key. `rename(from, to)` keeps the position, and returns false when `from` is missing or `to` exists.

### 2.3 Connection to the decoder

- `toValue(doc, node)` deep-copies a parsed subtree. Because `set` replaces, a repeated key in the source keeps its last value in the first member's position.
- `writeJson(JsonWriter&, JsonValue)` writes it canonically, as `writeJson` does for a document.
- `toDocument(value)` moves the value into shared storage and builds a `JsonDocument` over it. Offsets are 0. `readStruct` reads that document like any other input, so upgraded data is validated the same way.

## 3. Registering upgrades

### 3.1 The API

- `TypeBuilder::upgrade(from, fn)` registers the step from version `from` to `from + 1`. `reflect::Upgrade` is `std::function<bool(JsonValue& value, std::string& error)>`. The function edits the value in place. Returning false rejects the data, and `error` gives the reason.
- Registration mistakes are recorded in `TypeRegistry::errors()`, like the others. A rejected upgrade is not stored:
  - `from` is 0 or at least the type's version: `upgrade from version N is outside [1, V)`;
  - a second upgrade from the same version: `is registered twice`;
  - an empty function: `has no function`.
- `TypeInfo::upgradeFrom(from)` returns the step, or null. `canUpgradeFrom(from)` is true when every step from `from` up to the current version is registered. The current version needs no steps, so `canUpgradeFrom(current)` is true.

### 3.2 lint and readsVersions

- `lint()` reports each missing step on a serialized type, for example `Marker: version 3 needs .upgrade(1, ...) so version-1 files still load`.
- The catalogue lists `readsVersions` for each type: the current version, and every older version with a complete path. A version-3 type with steps 1 and 2 lists `[1, 2, 3]`. With only step 2 registered, it lists `[2, 3]`.

### 3.3 Existing types

The step-1 fixture `Everything` (version 2) gained `.upgrade(1, ...)`, which renames `dist` to `f` when it is present. Lint flagged it once the rule existed. No engine component has an upgrade, because every engine component is still version 1.

## 4. Upgrades on load (`ohao/ecs/scene_io.cpp`)

### 4.1 Header-driven choice

Phase 1 compares each header entry with the registered version:

- **Current version, or no header entry:** decoded directly. Current files never enter the upgrade code.
- **Older, with a complete path:** each block is copied into a `JsonValue`, run through the steps from its version to the current one, and decoded by `readStruct` on a side document.
- **Newer, or older with no path:** `E_COMPONENT_VERSION` at `/components/<name>`. `expected` lists the readable versions, for example `one of: 1, 2, 3`. Blocks of that type are skipped, so the error is reported once.

### 4.2 Errors from an upgraded block

- A typed-decoder error keeps its JSON Pointer path, for example `/entities/0/TestLamp/intensity`. Its offset and line come from the original block, and its message ends with ` (after upgrading TestLamp from version 1)`.
- Warnings keep their path and move to the original block's offset. Pending entity references are kept for phase 2.
- A rejecting upgrade stops that block and gives `E_UPGRADE` at the component path. Its message is `TestLamp upgrade 1 -> 2 failed: <reason>`, `expected` names the version the data was written as, and `got` is the block text.

### 4.3 Phase and reports

- All of this is phase 1. A failed or rejected upgrade leaves the world untouched. Phase 2 creates entities from the decoded values, as it does for current files.
- `LoadReport::upgraded` and `ValidationReport::upgraded` hold `UpgradeApplied {component, from, to, count}`, one entry per component type in registration order. `count` is the number of blocks upgraded. `validateScene` reports what a load would do.
- `saveScene` writes the current version for each type, so a saved file is current.

### 4.4 Error table

`errorCodes()` has 21 codes. `E_UPGRADE` sits after `E_COMPONENT_VERSION`.

## 5. The v1 corpus (`tests/scenes/v1`)

The three files are document version 1 with component version 1. They are never regenerated, and every future build must load them with the values the test asserts (`t08_v1_corpus_loads_with_expected_values`).

- `rooms.ohscene`: a canonical file with a header. Four entities: Room, Lamp (a spot light, child of Room), Sun (a directional light, with its rotation in `[x, y, z, w]` order) and Shade (child of Lamp).
- `agent_minimal.ohscene`: no header, members in free order, and an area-rect light with a child. The third entity has only an id, so it takes defaults.
- `preserved.ohscene`: an `AudioSource` block with header version 2. The engine does not register that type, so the block is kept with its value and version.

The corpus guards the document format and the current component versions. It does not exercise an upgrade step until a component version goes above 1.

## 6. Tests

### 6.1 scene_tests: 28/28

Step 3 left 23 tests. Step 4 adds five t08 tests. They use `TestLamp`, a test-only component at version 3 with two upgrades: from 1, `power` is renamed to `intensity` (a missing `power` needs no change), and from 2, `warm` and `cool` become `Warm` and `Cool`.

- **t08_registrations_have_every_upgrade:** engine types lint clean. A version-3 type with both upgrades lints clean. Without the step-1 upgrade, lint reports it. `readsVersions` is `[1, 2, 3]`, or `[2, 3]` without step 1.
- **t08_old_components_are_upgraded_on_load:** a version-1 file loads with both steps applied. The report has one entry, 1 to 3, with count 2. The saved output writes version 3 and round-trips with no further upgrade. A version-2 file runs only the remaining step.
- **t08_failed_upgrades_and_their_errors:** an unknown mode gives one `E_UPGRADE` at `/entities/0/TestLamp`, with its reason, and the world stays empty. A bad value found after upgrading keeps its path and points at the original line, with "after upgrading".
- **t08_newer_or_unreachable_versions_are_rejected:** version 4 gives `E_COMPONENT_VERSION` at `/components/TestLamp`, with `expected` `one of: 1, 2, 3`. Version 1 with no step 1 is rejected once, at the header. Version 2 still loads.
- **t08_v1_corpus_loads_with_expected_values:** the three files load, with the values in section 5.

### 6.2 reflect_tests: 28/28

Three t08 tests are new:

- `t08_json_value_edits`: rename keeps position and refuses to overwrite; `asFloat` and `asInt` read numbers only; erase; `set` replaces in place; numbers are canonical tokens; a non-finite number becomes null.
- `t08_upgraded_values_decode_through_the_typed_path`: a value-built document decodes through `readStruct`, and an out-of-range value gives `E_RANGE`.
- `t08_upgrade_registration_is_checked`: version 0, the current version, a duplicate and an empty function are each rejected with the messages in 3.1, and lint reports the missing step.

### 6.3 Goldens

`tests/scene/golden/engine_catalogue.json` and `tests/reflect/golden/fixture_catalogue.json` were regenerated for `readsVersions` and `E_UPGRADE`. The diffs are additive.

## 7. Verification

- `engine_tests` 58/58; `ecs_tests` 26/26; `runtime_tests` 21/21; `renderer_test` all pass.
- Physics 59/59; force generators 54/54; `check_scene_schema.py` 14/14.

No rendering code changed, so the render goldens were not re-run in this step. They passed at step 3, on the same rendering code.

Not run: the windowed `OHAO_VALIDATION=1 ohao_game` run. The session's tool permissions refused environment-prefixed commands.

## 8. Performance

Informational, as at step 3: release build, about ±30% run-to-run noise, nothing gated. Current-version files take no new work. The upgrade lookup is skipped when the header has no older versions.

The same 10k-entity benchmark, against the step 3 ranges:

| Operation | Step 3 | Step 4 |
|---|---|---|
| Save, pretty | 8.6 to 11.1 ms | 8.4 ms |
| Save, compact | 4.8 to 5.3 ms | 5.7 ms |
| JSON parse, warm parser | 4.2 to 5.4 ms | 4.1 ms |
| Validate | 14.3 to 15.1 ms | 13.1 ms |
| Load, including world teardown | 17.5 to 18.3 ms | 15.1 ms |

The values are no worse than step 3 beyond the noise. The budget work remains in step 6.

## 9. Spec adjustments made in this step

- The "Versioning and migration" section is detailed: the API, load behavior, reports and corpus files.
- `readsVersions` is described in the catalogue section.
- `E_UPGRADE` is in the error table.
- Step 4 is marked done in the build order.

## 10. Deferred to later steps

- Commands, query and the method table: step 5.
- Snapshots and the gated budgets: step 6.
- `MeshSource`, `MaterialOverride`, `Camera` and `Environment`, with their variant decoding in real scenes: steps 7 and 8.
- No engine component has a version above 1 yet. The first real migration comes with the first real component change.
