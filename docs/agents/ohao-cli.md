# The `ohao` CLI — a guide for agents

`ohao` reads, checks, edits, steps and renders OHAO scenes without an editor. Everything
it does goes through the engine's one API (`ohao/api`, the `Session` method table), so
what works here works the same over the future JSON-RPC/MCP server: a script line is a
JSON-RPC request without `jsonrpc`/`id`.

Every command below was run by `python tests/docs/check_docs.py`, and every output shown
is what it printed. Paths are relative to the repo root; the binary is
`build/Release/ohao.exe` (Git Bash finds it as `build/Release/ohao`).

## The contract

- **stdout is JSON, always.** One document per invocation: `{"result": …}` or
  `{"error": {"code", "message", "data": [diagnostics]}}`. Engine logs go to stderr.
  `run --script` prints one JSON line per operation instead.
- **Exit codes:** 0 ok · 1 validation, usage or input error · 2 asset or instantiate
  error (`E_ASSET_NOT_FOUND`, `E_ASSET_INVALID`, `E_MESH_INVALID`) · 3 GPU or internal
  error (`E_RENDER`, `E_INTERNAL`).
- **No GPU unless something renders.** validate, fmt, apply, query, summary and run
  without `--render` never create a Vulkan device.
- **Deterministic.** The same build, inputs and `--seed` give byte-identical stdout and
  files. Systems only see the fixed `dt`.
- **Diagnostics** carry `code`, `path` (a JSON Pointer into the file or the command
  batch), `entity`, `line`/`column` for files, `expected`, `got` and often a
  `suggestion`. At most 20 per reply (100 for `validate --limit`).

## Commands

```
ohao schema   [-o DIR]
ohao validate SCENE [--check-assets] [--limit N]
ohao fmt      SCENE [--check] [--compact] [-o OUT]
ohao apply    SCENE COMMANDS.json [-o OUT]
ohao query    SCENE [--ids a,b] [--name N] [--name-contains S] [--has T,U] [--fields T,T/f]
                    [--limit N] [--offset N] [--defaults]
ohao summary  SCENE [--depth D] [--max-children N] [--max-nodes N]
ohao run      SCENE [--seed S] [--steps N [--dt S]] [--script OPS.jsonl]
                    [--render OUT.png [--mode deferred|rt_realtime|rt_offline] [--spp N]
                     [--frames N] [--denoise none|oidn|nrd|atrous|dlss] [--size WxH]
                     [--post bloom,taa,ssao] [--exposure E]]
                    [--save OUT.scene.json]
ohao doctor   [--cpu-only] [--shader-dir DIR] [--spirv-dir DIR]
every scene command: [--root DIR]  the project root that asset paths resolve against (default .)
```

**Git Bash rewrites arguments that start with `/`** into Windows paths
(`/Transform` reaches the program as `C:/Program Files/Git/Transform`). Write `--fields`
without the leading slash, as below; `ohao` adds it. JSON inside files and scripts is not
affected.

### Orient yourself first: summary, then query

A summary costs a few hundred tokens whatever the scene's size: counts per component,
the bounds of entity origins and a capped outline.

```console
$ build/Release/ohao summary assets/scenes/cornell_box.scene.json --depth 1 --max-nodes 6
{"result":{"entities":21,"roots":21,"components":{"Name":21,"Transform":21,"Light":12,"MeshSource":7,"MaterialOverride":7,"Camera":1,"Environment":1,"Children":21,"CreationOrder":21},"bounds":{"min":[-4,-3.2,-4],"max":[4,4,13]},"outline":[{"id":"77c2d2f9c6f089b6","name":"MainCamera"},{"id":"1a0edc87612c3fd5","name":"Environment"},{"id":"2dbe393e136129ec","name":"Back"},{"id":"dd80bf9bc5d8c7eb","name":"Left"},{"id":"bef33aced9e453c8","name":"Right"},{"id":"616133e14807f333","name":"Floor"}],"moreRoots":15}}
```

Then ask for exactly what you need. Filters combine (all must match); `--fields`
projects `Type` or `Type/field`. Without `--fields` each entity comes back in its file
form, defaults omitted (`--defaults` writes them).

```console
$ build/Release/ohao query assets/scenes/cornell_box.scene.json --name-contains Sphere --fields Transform/position,MaterialOverride/roughness
{"result":{"entities":[{"id":"499421b6ef3e75a9","Transform":{"position":[-2,-3,0]},"MaterialOverride":{"roughness":0.05}},{"id":"b1ab29372b0e7edc","Transform":{"position":[2.5,-3.2,1.5]},"MaterialOverride":{"roughness":0.02}}],"count":2}}
```

### Validate before you load anything you wrote

`validate` checks a file exactly as loading would, and reports every error, not the
first. `--check-assets` also checks that model and HDRI files exist.

```console
$ build/Release/ohao validate assets/scenes/damaged_helmet.scene.json --check-assets
{"result":{"ok":true}}
$ printf '{"format":"ohao.scene","version":1,"entities":[{"id":"00000000000000a1","Spin":{"degreesPerSec":45}},{"id":"00000000000000a2","Light":{"intensity":-1}}]}' > build/doc_tmp/typos.scene.json
$ build/Release/ohao validate build/doc_tmp/typos.scene.json   # exit 1
{"result":{"ok":false,"errors":[{"code":"E_UNKNOWN_FIELD","path":"/entities/0/Spin/degreesPerSec","entity":"00000000000000a1","line":1,"column":97,"message":"Spin has no field 'degreesPerSec'","expected":"one of: axis, degreesPerSecond","got":"\"degreesPerSec\"","suggestion":"degreesPerSecond"},{"code":"E_RANGE","path":"/entities/1/Light/intensity","entity":"00000000000000a2","line":1,"column":148,"message":"Light.intensity is outside its limits","expected":">= 0","got":"-1"}]}}
```

Both mistakes are reported at once, each with where it is and what was expected.

### Edit with commands, not by rewriting the file

`apply` runs one atomic batch of commands (see "Edit commands" below) and saves the
result in canonical form. If any command fails, nothing changes and the reply names the
command (`/commands/N/...`).

```console
$ printf '[{"op":"entity.duplicate","id":"499421b6ef3e75a9","name":"MetalSphere 2"},{"op":"field.set","id":"$0","path":"/Transform/position/2","value":2},{"op":"component.set","id":"$0","type":"Spin","value":{"degreesPerSecond":30}}]' > build/doc_tmp/edit.json
$ build/Release/ohao apply assets/scenes/cornell_box.scene.json build/doc_tmp/edit.json -o build/doc_tmp/edited.scene.json --seed 0000000000000001
{"result":{"apply":{"applied":3,"results":["910a2dec89025cc1",null,null],"inverse":[{"op":"component.remove","id":"910a2dec89025cc1","type":"Spin"},{"op":"field.set","id":"910a2dec89025cc1","path":"/Transform/position/2","value":0},{"op":"entity.destroy","id":"910a2dec89025cc1"}]},"save":{"path":"build/doc_tmp/edited.scene.json","bytes":6228}}}
$ build/Release/ohao query build/doc_tmp/edited.scene.json --name "MetalSphere 2"
{"result":{"entities":[{"id":"910a2dec89025cc1","Name":{"value":"MetalSphere 2"},"Transform":{"position":[-2,-3,2],"scale":[2,2,2]},"MeshSource":{"source":{"SpherePrim":{"radius":0.5,"sectors":32,"stacks":16}}},"MaterialOverride":{"baseColor":[0.95,0.93,0.88],"roughness":0.05,"metallic":1},"Spin":{"degreesPerSecond":30}}],"count":1}}
```

`results[0]` is the copy's id; `inverse` is a batch that undoes this one exactly, last
command first. `--seed` fixes the ids new entities get; without it they are random.

### Step, script, snapshot and restore

`run` loads a scene, then steps (`--steps`), runs a script, renders and saves, in that
order. A script is JSON Lines, one `{"method", "params"}` per line, each answered by
one output line; every line runs, and the exit code is the worst one seen.

```console
$ printf '%s\n' '{"method":"world.snapshot","params":{"name":"base"}}' '{"method":"world.step","params":{"frames":30,"dt":0.0333333}}' '{"method":"world.query","params":{"names":["MetalSphere 2"],"fields":["/Transform/rotation"]}}' '{"method":"world.restore","params":{"name":"base"}}' > build/doc_tmp/spin.jsonl
$ build/Release/ohao run build/doc_tmp/edited.scene.json --script build/doc_tmp/spin.jsonl
{"method":"scene.load","result":{"entities":22}}
{"method":"world.snapshot","result":{"name":"base","entities":22,"bytes":4080}}
{"method":"world.step","result":{"frame":30}}
{"method":"world.query","result":{"entities":[{"id":"910a2dec89025cc1","Transform":{"rotation":[0,0.25881878,0,0.96592593]}}],"count":1}}
{"method":"world.restore","result":{"created":0,"destroyed":0,"changed":1,"frame":0}}
```

One second at 30°/s turned the copy 30° about +Y, and the restore put back exactly the
one Transform that changed. Snapshots live in memory for the session; they are the cheap
way to try an edit and go back.

### Canonical form

Scene files have one canonical form: keys in registration order, defaults omitted, two
spaces, short containers on one line, shortest round-tripping floats. Save, load, save is
byte-identical. `fmt` rewrites a file into it (`--compact` for one line), `--check` only
reports.

```console
$ build/Release/ohao fmt assets/scenes/cornell_box.scene.json --check
{"result":{"path":"assets/scenes/cornell_box.scene.json","canonical":true}}
$ build/Release/ohao fmt build/doc_tmp/edited.scene.json --compact -o build/doc_tmp/edited.compact.json
{"result":{"path":"build/doc_tmp/edited.compact.json","changed":true,"bytes":4138}}
```

### Render

Rendering needs a GPU. The first capture starts the renderer (a second or so); `ms` in
the result includes that. Without `--post`, a deferred capture renders as `cornell_box`
does; `model_viewer` turns on `--post taa,ssao,bloom --exposure 1`.

```console
$ build/Release/ohao run assets/scenes/cornell_box.scene.json --render build/doc_tmp/cornell.png --mode deferred --size 960x540   # gpu
$ build/Release/ohao run assets/scenes/cornell_box.scene.json --render build/doc_tmp/cornell_rt.png --mode rt_offline --spp 16 --denoise none   # gpu
```

A model that cannot be built is an error with exit code 2, before any frame:

```console
$ printf '{"format":"ohao.scene","version":1,"entities":[{"id":"00000000000000b1","MeshSource":{"source":{"ModelAsset":{"asset":{"path":"assets/nope.glb"}}}}}]}' > build/doc_tmp/missing.scene.json
$ build/Release/ohao run build/doc_tmp/missing.scene.json --render build/doc_tmp/missing.png --size 64x64   # exit 2 # gpu
{"method":"render.capture","error":{"code":"E_ASSET_NOT_FOUND","message":"entity 00000000000000b1: no file 'assets/nope.glb' in the project","data":[{"code":"E_ASSET_NOT_FOUND","path":"","entity":"00000000000000b1","message":"entity 00000000000000b1: no file 'assets/nope.glb' in the project"}]}}
```

### The schema

`ohao schema` writes `ohao_schema.json` (every component, field, unit, limit, enum,
error code and method, with docs) and `scene.schema.json` (a JSON Schema for scene
files). Read the catalogue once and key your cache on `schemaHash`; it changes whenever
anything in it does. The reply is `{"result": {"files": [...], "schemaHash": "…"}}`.

```console
$ build/Release/ohao schema -o build/doc_tmp/schema
$ python -c "import json; c = json.load(open('build/doc_tmp/schema/ohao_schema.json')); print(sorted(c))"
['enums', 'errors', 'format', 'methods', 'schemaHash', 'types', 'units', 'version']
```

### The environment

When something fails in a way that does not look like your change — no device, a render
that takes seconds, a shader that behaves like an old one — ask `ohao doctor` before
anything else. Each check is `{id, ok, level, detail, fix?}`; read the failing ones'
`fix`. `level` `error` means OHAO cannot work here (exit 1), `warn` that something
degrades. `--cpu-only` skips the device checks.

```console
$ build/Release/ohao doctor --cpu-only > build/doc_tmp/doctor.json
$ python -c "import json; r = json.load(open('build/doc_tmp/doctor.json'))['result']; print([c['id'] for c in r['checks']])"
['shaders.complete', 'shaders.fresh', 'assets.present', 'tools.glslc', 'tools.cmake', 'python.deps']
```

## Methods

What a script line or (later) an RPC request can call. Params with `=` have that default.

| Method | Params → result | What it does |
|---|---|---|
| `schema.get` | `{}` → the catalogue, compact | Every component, field, enum, limit, unit, error code and method. Read once; key on `schemaHash`. |
| `scene.validate` | `{text \| path \| scene, limit = 20, checkAssets = false}` → `{ok, errors?, warnings?, upgraded?, more?}` | Checks a scene exactly as `scene.load` would, without touching the world. |
| `scene.load` | `{text \| path \| scene, seed?}` → `{entities, preserved?, upgraded?, warnings?}` | Loads into the session's world, which must be empty. All or nothing. `seed` (16 hex digits) fixes the id generator. |
| `scene.save` | `{path?, layout = compact \| pretty, defaults = false}` → `{scene}` or `{path, bytes}` | The canonical scene; without a path it is returned as an object. |
| `world.info` | `{}` → `{entities, frame, schemaHash, idCounter}` | Cheap orientation. |
| `world.summary` | `{depth = 2, maxChildren = 8, maxNodes = 64}` → `{entities, roots, components, bounds?, outline, moreRoots?}` | Counts, bounds and a capped hierarchy outline; a node with unlisted children carries `more`. |
| `world.query` | `{ids?, names?, nameContains?, has?, fields?, limit = 50, offset = 0, defaults = false}` → `{count, entities, missing?, next?}` | Entities in creation order, filtered and projected (`"/Type"`, `"/Type/field"`). |
| `world.apply` | `{commands}` → `{applied, results, inverse}` | One atomic batch; on any error nothing changes. |
| `world.step` | `{frames = 1, dt = 1/60}` → `{frame}` | Runs the world's systems with a fixed `dt`. |
| `world.snapshot` | `{name}` → `{name, entities, bytes}` | Captures the world in memory under a name. |
| `world.restore` | `{name}` → `{created, destroyed, changed, frame}` | Back to a snapshot, writing only what differs. The snapshot is kept. |
| `render.capture` | `{path, mode = deferred, spp = 16, frames?, width = 1920, height = 1080, denoise = none, post = [], exposure?}` → `{path, frames, ms, validation?}` | Renders through the world's Camera and Environment to a PNG (needs `ohao run`, a GPU). |

## Edit commands

The members of a `world.apply` batch. An entity is named by its id (16 lowercase hex
digits) or by `"$k"`, the entity command `k` of the same batch created. `results[k]` is
that id.

| Op | Members | Effect | Inverse |
|---|---|---|---|
| `entity.create` | `id?, name?, parent?, components?, order?, preserved?` | A new entity; `components` maps component names to values (missing fields take defaults). | `entity.destroy` |
| `entity.duplicate` | `id, name?, parent?, recursive = true` | A copy of the entity and its subtree with fresh ids, under `parent` (default: the source's parent). A subtree with a Camera or Environment cannot be copied. | `entity.destroy` of the copy |
| `entity.destroy` | `id, recursive = true` | Destroys the entity and its subtree (`recursive: false` detaches the children instead). | `entity.create` per entity, in order |
| `entity.reparent` | `id, parent` | `parent: null` makes it a root. Cycles are refused (`E_PARENT_CYCLE`). | `entity.reparent` |
| `component.set` | `id, type, value` | Adds or replaces a saved component (missing fields take defaults). | `component.set` / `component.remove` |
| `component.remove` | `id, type` | Removes a saved component. `Name` and `Transform` always exist. | `component.set` |
| `field.set` | `id, path, value` | One field, by path: `/Type/field`, then member names or array indices (`/Transform/position/1`). Limits are checked (`E_RANGE`). | `field.set` |

## Components

`ohao schema` is the authority; this is the map. Saved (authoring) components:

| Component | What it is |
|---|---|
| `Name` | Display name (not unique; ids identify entities). |
| `Transform` | Local position, rotation (quaternion `[x, y, z, w]`), scale, relative to the parent. |
| `Parent` | The parent entity's id. |
| `Light` | Point, Directional, Spot or AreaRect light; direction is the entity's −Z. |
| `MeshSource` | Where the mesh comes from: `ModelAsset` (a glb/gltf/fbx/obj path) or a primitive (`SpherePrim`, `CubePrim`, `PlatformPrim`, `CylinderPrim`, `ConePrim`, `QuadPrim`, `BoxPrim`). |
| `MaterialOverride` | The material of a primitive: baseColor, roughness, metallic, emissiveStrength. |
| `Camera` | The scene camera (one per world); looks along −Z of its entity. |
| `Environment` | The HDRI that lights the scene (one per world). |
| `Spin` | Turns the entity about a local axis at `degreesPerSecond` (the spin system, every step). |

Runtime components (`WorldTransform`, `MeshRenderer`, `RenderSlot`, …) are derived each
frame, appear in the catalogue for reference, and are refused in files and commands.

## Error codes

| Code | Meaning |
|---|---|
| `E_IO` | The file could not be read or written. |
| `E_PARSE` | Not valid JSON (or not valid UTF-8). |
| `E_FORMAT` | Not an OHAO scene: `format`, `version` or `entities` is wrong or missing. |
| `E_VERSION_UNSUPPORTED` | A document version this engine does not read. |
| `E_COMPONENT_VERSION` | A component version newer than the engine's, or older with no upgrade path. |
| `E_UPGRADE` | An upgrade rejected a component written by an older version. |
| `E_UNKNOWN_FIELD` | A member the type does not have (often with a `suggestion`), or a runtime component in a file. |
| `E_DUPLICATE_KEY` | The same member twice in one object. |
| `E_TYPE` | A value of the wrong JSON type or shape. |
| `E_ENUM` | A string that is not a value of the enum. |
| `E_RANGE` | A number outside the field's limits or its type's range. |
| `E_NONFINITE` | NaN or infinity. |
| `E_ID_INVALID` | A missing, malformed or zero id (ids are 16 lowercase hex digits). |
| `E_ID_DUPLICATE` | Two entities with the same id. |
| `E_PARENT_MISSING` | A Parent that names no entity. |
| `E_PARENT_CYCLE` | Parents that form a cycle. |
| `E_SINGLETON` | A second Camera or Environment. |
| `E_ENTITY_NOT_FOUND` | An entity reference (other than Parent) that names no entity. |
| `E_WORLD_NOT_EMPTY` | Loading into a world that already has entities. |
| `E_TOO_MANY_ERRORS` | More errors exist than are reported. |
| `E_METHOD` | An unknown method (with a `suggestion`). |
| `E_COMMAND` | A malformed request or one that cannot apply. |
| `E_COMPONENT_UNKNOWN` | Not a component, or (for edits) not a saved one. |
| `E_PATH` | A path that reaches no field. |
| `E_SNAPSHOT_UNKNOWN` | No snapshot has this name. |
| `E_ASSET_NOT_FOUND` | An asset path that names no file in the project. Exit code 2. |
| `E_ASSET_INVALID` | An asset file that cannot be loaded. Exit code 2. |
| `E_MESH_INVALID` | A MeshSource that gives no usable geometry. Exit code 2. |
| `E_RENDER` | `render.capture` failed: no GPU, a failed frame, an unwritable image. Exit code 3. |
| `E_INTERNAL` | A bug in a tool; the operation did not complete. Exit code 3. |
| `W_UNKNOWN_COMPONENT` | Warning: an unknown component close to a known name, kept but probably a typo. |
| `W_UNSAVEABLE_MESH` | Warning: a mesh made in code, with no MeshSource, is not saved. |

## Habits that save tokens and mistakes

- `summary` once, then `query` with `--fields`; never read a whole scene to change one value.
- Build edits as one batch; use `"$k"` to edit what the batch just created; keep the
  `inverse` if you may want to undo.
- `validate` anything you wrote by hand before loading it, and act on `suggestion`.
- Snapshot before an experiment, restore after; it is far cheaper than reloading.
- Key any cached catalogue on `schemaHash`.
