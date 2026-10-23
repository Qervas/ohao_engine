# Sub-project 3, build step 5: the API, commands, queries and the token budget

**Spec:** `docs/specs/2026-10-22-reflection-scene-serialization-design.md` (build order, step 5; "API layer" and "Token budget (agents)")
**Status:** Done; committed in `5d72ea2` (the file rename) and `41a1e92` (local, not pushed).

## 1. Summary

Step 5 adds `ohao_api` (`ohao/api`), the one dispatch for authoring operations. `api::Session::call(method, params)` takes a JSON object and returns compact JSON: a result or a terse error. Nine methods are implemented. Edits go through commands, and each command returns its inverse. A failing batch is rolled back. Canonical scene files now leave out fields equal to their default. Queries filter and project, so an agent reads only what it asks for. Scene files move to the `.scene.json` suffix in the same step.

## 2. File naming (`5d72ea2`)

Scene files are named `<name>.scene.json`, and the JSON Schema is `scene.schema.json` (`urn:ohao:scene:1`). The reasons, from the spec's "File naming" section:

- Editors, diff tools, `jq` and JSON Schema validators read `.json` with no setup. Editors can bind `scene.schema.json` to `**/*.scene.json`.
- The root member `"format": "ohao.scene"` decides what a file is. The suffix is a hint for tools and people.
- Compact and pretty are one format, so they share the suffix. The cooked binary, when it comes, is `<name>.scene.bin`.
- Rejected: YAML (slower to parse, implicit typing, more ways to write an invalid file), and the first choice `.ohscene` (no tooling at all).

OpenUSD import and export is planned as roadmap sub-project 8. USD keeps its own extensions (`.usda`, `.usdc`, `.usd`), and `.scene.json` stays the native authoring format.

Old files are still tracked. The 21 `tests/scenes/**/*.ohscene` files and two goldens, `tests/reflect/golden/fixture_ohscene.schema.json` and `tests/scene/golden/engine_ohscene.schema.json`, were left in place because this session cannot delete files. Nothing reads them. Frank removes them with:

```
git rm -r --quiet -- '*.ohscene' tests/reflect/golden/fixture_ohscene.schema.json tests/scene/golden/engine_ohscene.schema.json
```

## 3. The API (`ohao/api`)

### 3.1 Session and the reply

- `api::Session(World&, const TypeRegistry&)` owns the dispatch. `call(method, params)` returns compact JSON, exactly one of `{"result": …}` or `{"error": {"code", "message", "data"}}`.
- `code` and `message` come from the first diagnostic. When there are more, the message ends with ` (+N more)`. `data` holds up to 20 diagnostics in the file-validation shape, with paths into the params, such as `/commands/3/value`.
- Empty params mean `{}`. An unknown member is `E_UNKNOWN_FIELD` with a suggestion. An unknown method is `E_METHOD` with a suggestion.
- The method table (`kMethods` in `ohao/api/session.cpp`) is exported as `"methods"` in the catalogue and is part of `schemaHash`. Its entries are `reflect::MethodDoc`.

### 3.2 The nine methods

| Method | Params | Result |
|---|---|---|
| `schema.get` | none | the catalogue, compact, with `methods` and `schemaHash` |
| `scene.validate` | `text` or `path` or `scene`; `limit` = 20 | `ok`, `errors?`, `warnings?`, `upgraded?`, `more?` |
| `scene.load` | `text` or `path` or `scene`; `seed?` | `entities`, `preserved?`, `upgraded?`, `warnings?`. The world must be empty; all or nothing. |
| `scene.save` | `path?`; `layout` = compact or pretty; `defaults` = false | `scene` (the compact scene as an object), or `path` and `bytes` |
| `world.info` | none | `entities`, `frame`, `schemaHash`, `idCounter` |
| `world.summary` | `depth` = 2, `maxChildren` = 8, `maxNodes` = 64 | counts, `roots`, `bounds?`, `outline`, `moreRoots?` |
| `world.query` | filters and projection (section 4.2) | `entities`, `count`, `missing?`, `next?` |
| `world.apply` | `commands` | `applied`, `results`, `inverse` |
| `world.step` | `frames` = 1, `dt` = 1/60 | `frame` |

- `scene` takes the scene as a JSON object, so an agent writes it with no string escaping. `text` is the same as a string and `path` a file. `seed` (16 hex digits) fixes the id generator for later creates.
- `checkAssets` is not accepted yet; it arrives with assets (step 7).
- Not in this step: `world.snapshot` and `world.restore` (step 6), and `render.capture` (step 9).

## 4. Commands and queries

### 4.1 Commands

Six ops: `entity.create`, `entity.destroy`, `entity.reparent`, `component.set`, `component.remove`, `field.set`.

- **Sequential validation.** Commands apply in order. Each is validated against the world as the earlier ones left it, so one batch can create an entity and then edit it. A command checks everything before it changes anything. Changes go through `emplace_or_replace`, `remove` and `World::setParent`, so the normal signals fire.
- **Rollback.** When a command fails, the commands already applied are undone by their inverses, last first. The id generator and the creation sequence are restored, so the world saves byte-identically to before the batch.
- **Revised in this step.** The first design validated the whole batch before applying any of it. That cannot check a command against entities an earlier command of the same batch creates, reparents or destroys. Validation moved into the sequential apply, with rollback.
- **`$k` references.** Command-level `id` and `parent` accept `"$k"`, the entity created by command `k` of the same batch (an earlier `entity.create`).
- **Results.** `results[i]` is the id command `i` created, or `null`.
- **Inverses.** Every applied batch returns its inverse. The inverse of a batch is the inverse lists of its commands, last command first. The inverse of an inverse redoes the batch. The inverses are: create to destroy; destroy to a create per entity in pre-order, plus reparents of detached children; reparent to reparent back; `component.set` to the old value or a `component.remove`; `component.remove` to a `component.set`; `field.set` to the old value at the same path.
- **Known limit.** An undo that recreates a child appends it to its parent's `Children`, so sibling order can differ after undo. Saves are not affected, because they order by `CreationOrder`, which is restored. Step 6's restore does not have this limit.
- `Name` and `Transform` are on every entity and cannot be removed (`E_COMMAND`). `Parent` changes always go through `World::setParent`, so cycles are `E_PARENT_CYCLE`.
- `field.set` paths are `/Type/field`, optionally followed by member names or indices. A deep path decodes the whole field through `reflect::readField`, so limits and types are checked as in files.

### 4.2 Queries

- **Filters.** All given filters must match: `ids` (any of), `names` (exact, any of), `nameContains` (substring), and `has` (every listed component, runtime ones included).
- **Paging.** Entities come in creation order. `limit` defaults to 50 and `offset` to 0. `count` is the total number of matches, and `next` is the following offset, present only when more remain. Ids that do not exist are listed in `missing`, not reported as an error.
- **Projection.** Without `fields`, each entity is its compact file form, with defaults left out unless `defaults: true`. With `fields: ["/Light", "/Transform/position"]`, a single field is always written. A whole-component projection (`/Light`) leaves out default fields unless `defaults: true`, as the file form does. `fields: []` gives ids only. Runtime components can be projected (`/WorldTransform/world`).

### 4.3 world.summary

- Counts per component, runtime components included, and the number of roots.
- `bounds`, the box of the entity origins in world space. It is composed from local transforms, so it is current before any step.
- `outline`, the hierarchy in creation order, `depth` levels below the roots. Each node is `{id, name?, children?, more?}`. `more` counts the direct children that were not listed, because `depth`, `maxChildren` or `maxNodes` was reached.

## 5. The token budget

Agents pay for every byte they read and write. An agent orients with `world.info` or `world.summary`, reads what it needs with filtered and projected `world.query`, edits with `field.set`, and reads terse errors.

### 5.1 Defaults are omitted in canonical files

- A top-level field equal to its registered default is left out. An always-present component (`Name`, `Transform`) that is entirely default is left out. A fully default optional component is still written, as `{}`, because its presence is the information. Nested values (inside arrays, optionals and variants) are written in full.
- Equality is bit-exact, so `-0` is written where the default is `0`.
- Why it stays canonical: omission is decided per value by the registered default. One world still gives one byte sequence, and save, load, save is byte-identical. `SaveOptions::includeDefaults` (and `scene.save {defaults: true}`) writes every field. It loads to the same world and re-saves to the canonical form.
- Why it helps: a missing field decodes to exactly its default, so nothing is lost. Diffs show only what an author changed. A typical light goes from 9 fields to 2 or 3. On the 10k test world, pretty output goes from 2.89 MB to 2.58 MB.
- **Hazard:** changing a registered default changes the meaning of every file that omitted that field. A default change must therefore be a component version bump whose upgrade writes the old default explicitly into old files. The v1 corpus asserts some defaults and catches an accidental change.

### 5.2 Terse errors

- One diagnostic is one line of JSON: code, path, expected, got and a suggestion.
- "One of" lists are cut after 12 names, with `, +N more`.
- An error reply carries at most 20 diagnostics. `scene.validate` returns at most `limit` (20) of each kind, with a `more` count.

### 5.3 Estimates

Measured on the 10k-entity test world (tokens ≈ bytes / 4):

| Operation | Request + reply | ≈ tokens |
|---|---|---|
| `world.query`, one entity, one projected field | 61 + 98 B | 40 |
| `world.apply`, one `field.set` (the reply includes its inverse) | 100 + 137 B | 60 |
| One error (for example a misspelled field, with a suggestion) | 289 B | 70 |
| `world.info` | ~90 B | 25 |
| `world.summary` of 10k entities (64 outline nodes) | 3.3 KB | 830 |
| `world.query`, 1,000 entities, full components | 122 KB | 31k |
| The whole 10k scene, compact (never needed) | 1.26 MB | 310k |

A typical edit loop (one summary, then query, edit and verify per change) costs about 1k tokens up front and 100 to 200 tokens per edit, whatever the scene size.

## 6. Other changes

- `reflect::readField(object, type, field, node, ctx)` (`ohao/reflect/decode.cpp`) decodes one field from a node through the typed decoder. The deep `field.set` path uses it.
- `TypeBuilder::alwaysPresent()` marks a component that every entity has (`Name`, `Transform`). It is left out of files when entirely default, and it cannot be removed.
- `reflect::MethodDoc` and the catalogue's `methods` table (section 3.1).
- Four new error codes: `E_METHOD`, `E_COMMAND`, `E_COMPONENT_UNKNOWN`, `E_PATH`. `errorCodes()` has 25 codes. Command targets use the existing `E_ENTITY_NOT_FOUND`.
- `ComponentOps` gained `remove` and `count`. Commands use `remove` for `component.remove`, and `count` for the singleton checks. `world.summary` uses `count` for its component counts.
- `saveScene` no longer drops saved component types past the 64th (the fix named in commit `41a1e92`). The first omission pass recorded each entity's written components in a 64-bit mask; it now keeps one pointer per entity and saved type (null when the component is absent or omitted).
- `ohao_api` is a static library linked to `ohao_ecs`, `ohao_reflect` and `ohao_unordered_dense`. Its public include root is `ohao/`.

## 7. Tests

### 7.1 api_tests: 12/12

Eleven `t09` tests and one benchmark, `bench_apply_and_query_10k`. They cover the method table and its errors, `schema.get` with methods, each command and its inverse, rollback of a failing batch, a batch followed by its inverse (byte-identical), a command-built world saving like the file, `$k` references, precise and terse command errors, query filters and projection, the summary, and the scene methods.

### 7.2 scene_tests: 30/30

Step 4 left 28 tests. Step 5 adds two omission tests:

- **t02_defaults_are_omitted_and_round_trip:** default fields, an empty `Name` and a default `Transform` are left out; a fully default `Light` is written as `{}`; `-0` is kept where the default is `0`. The omitted form loads back to the same world and saves byte-identically, and pretty is still `prettyPrint(compact)`.
- **t02_include_defaults_writes_every_field:** `includeDefaults` writes every field, and the output loads and re-saves to the canonical form.

### 7.3 Verification

- `api_tests` 12/12; `scene_tests` 30/30; `reflect_tests` 28/28; `check_scene_schema.py` 14/14.
- `ecs_tests` 26/26; `engine_tests` 58/58; `runtime_tests` 21/21; `force_generator_tests` 54/54; `physics_backend_tests` all passed.
- `renderer_test` ALL PASS. It prints a line "OIDN failed — returning noisy pixels"; no renderer or denoiser code changed in this step.

No rendering code changed, so the render goldens were not run.

Not run: the windowed `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200` check. The session's tool permissions refused environment-prefixed commands. Frank must run it.

## 8. Performance

Informational. Release build on this machine. Nothing is gated in this step; step 6 gates the budgets.

| Operation | Measured | Step-6 budget |
|---|---|---|
| `world.apply`, batch of 100 `field.set` (median) | 0.08 to 0.12 ms | 0.2 ms |
| `world.query`, 1,000 ids, full components, compact (median) | 0.46 to 0.63 ms; 122 KB reply | 2 ms |
| `world.summary`, 10k entities | ~0.9 to 1.1 ms | none |
| Save, pretty (10k test world) | 8.4 ms; 2.58 MB (2.89 MB with defaults) | 10 ms |
| Save, compact | 4.7 ms | none |
| Validate | 13.0 ms | 10 ms |
| Load, including world teardown | 15.3 ms | 20 ms |

The budgets are for the about-5 MB `bench10k` scene (spec "Budgets"), and the test world here is 2.58 MB, so the comparison is only indicative. Validate is over its 10 ms budget. Load is under 20 ms. Both are left for step 6.

## 9. Spec adjustments made in this step

- "File naming" describes `.scene.json`, and the rename is recorded in the decisions table (decision 4).
- "API layer" describes the reply envelope, the nine methods, the command rules and the sequential-validation revision.
- "Token budget (agents)" gives the rules and the estimate table.
- The `ohao` CLI list gains `query` and `summary` (step 9), and decision 19 is updated to match.
- Step 5 is marked done in the build order.

## 10. Known gaps and deferred

- The windowed `OHAO_VALIDATION=1` run (section 7.3) is not done. Frank runs it.
- `schema.get` has no per-type filter yet.
- `checkAssets` arrives with assets (step 7).
- The CLI `ohao query` and `ohao summary` come in step 9.
- Undo can change sibling order (section 4.1).
- `world.snapshot` and `world.restore` are step 6, and `render.capture` is step 9.
- Validate is over its step-6 budget (section 8).

Next: step 6 (columnar snapshot/restore, scene_bench and the budgets).
