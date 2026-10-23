# Sub-project 3 — Reflection, scene serialization and the agent API

Status: design approved in conversation 2026-10-22, revised the same day for three
requirements:
- AI agents are first-class users;
- performance is a first-class constraint;
- each layer gets the best language for it, efficiency first.

Frank's answers to the three open decisions were folded in the same day.

This document is the binding spec. No decisions are open.
Roadmap: first step of the general-purpose engine track (editor, project/asset database,
scripting, agent control surface). Sub-project 4 is the agent control surface plus agent
dev ergonomics. The level pipeline, player and combat move to 5, 6 and 7.

## Thesis: an AI-native engine

Today's engines (Godot, Unity, Unreal) are built around a human in an editor, and
agents get access afterwards through plugins that drive that editor. OHAO turns this
around. The agent surface is the foundation, and the editor is one more client of it:
- typed commands with undo, shared by the editor and agents;
- canonical scene files described by a published schema;
- deterministic headless runs;
- one protocol across CLI, RPC/MCP and the C ABI.

Everything in this spec serves that thesis.

## Goal

The engine knows the fields of every saveable component, and a whole `ecs::World`
moves between memory and a canonical text file exactly and fast. One API layer covers
every authoring operation:
- load and validate;
- query;
- apply edit commands;
- step the world;
- snapshot and restore;
- save;
- render to PNG.

A human editor (later) and an AI agent (now, through a CLI) call that same layer. The
type registry behind it costs nothing on per-frame hot paths.

**Done when:**
- `cornell_box` and the helmet scene exist as checked-in `.scene.json` files.
- `ohao run` renders them headlessly and all five goldens pass from the files.
- An agent can do everything in "Agents as users" through the `ohao` CLI without
  reading C++.
- Save → load → save is byte-identical.
- The benchmark budgets in "Performance" are met on the reference machine.

## Agents as users

OHAO's users are humans and AI agents, and neither is a second-class client of the
other. "Agentic" has two explicit goals:

- **G1. Agents build games with OHAO end to end:**
  - create and edit scenes;
  - run them, playtest, observe and iterate;
  - eventually write gameplay code and ship.
- **G2. Agents develop OHAO itself easily:**
  - one obvious way to build and test, with a clear pass/fail;
  - machine-readable results;
  - an accurate map of the code;
  - a known place and recipe for every common change.

Sub-project 3 delivers the foundation of G1: the five properties below. Its share of G2
is in "Engine development by agents". Sub-project 4 (the agent control surface) carries
both goals further.

For sub-project 3, G1 means five properties:

1. **Discoverable without C++.** `ohao schema` emits a complete, machine-readable
   description of every component, field, type, default, hard limit, unit, enum value,
   one-line doc string, singleton rule and version. It also emits a standard JSON Schema
   (draft 2020-12) that any language's validator can check a scene file against. A
   registration without docs fails a test.
2. **Safe to read, write and diff.**
   - Scene files are canonical: one world gives exactly one byte sequence.
   - Entity IDs are stable.
   - Every field is written, so a file explains itself.
   - Every validation error is machine-readable:
     - a stable error code;
     - a JSON Pointer path;
     - the entity ID;
     - the line and column;
     - expected and actual values;
     - "did you mean" suggestions for misspelled names.
   - Validation reports all errors in one pass, not just the first.
3. **Edits are commands, not file rewrites.** An agent changes a world with small typed
   commands (`field.set`, `entity.create`, …) that are validated, atomic in batches and
   return their own inverse. The future editor uses exactly these commands. Undo, agent
   rollback and human edits are one mechanism.
4. **Headless and deterministic.**
   - `ohao run` loads a scene, steps N fixed-dt frames, queries and patches the world,
     snapshots and restores, saves and renders to PNG.
   - It runs from flags or from a JSON-Lines operation script, with no window, and needs
     no GPU unless a render is requested.
   - World state (everything except pixels) is bit-identical across runs of the same
     build with the same inputs.
5. **One protocol, several transports.** A CLI script line is a JSON-RPC 2.0 request
   without the envelope. The method names, parameters, results and error objects are
   fixed in this spec. The control surface in sub-project 4 (`ohao serve`: JSON-RPC 2.0
   over stdio, which MCP clients speak natively) only adds a transport; it adds no new
   semantics.

## Scope: in 3 vs later

| In sub-project 3 | Later (designed for here, built in its own sub-project) |
|---|---|
| `ohao_reflect`: type registry, docs/units/limits, `ohao_schema.json` + JSON Schema export | Editor UI (ImGui), picking, gizmo interaction, undo stack UI |
| `EntityId`, `findById` | **Sub-project 4:** `ohao serve` (JSON-RPC 2.0 / MCP over stdio, then a named pipe), plus the agent dev ergonomics listed in "Engine development by agents" |
| `AGENTS.md`, `docs/ARCHITECTURE.md`, refreshed `.claude/skills` (see "Engine development by agents") | |
| Canonical text codec (pretty + compact), two-phase load, structured validation | C ABI `ohao_c.h` for in-process embedding; zero-copy bulk observations |
| Versioning, migrations, v1 corpus | Cooked binary scene format (`ohao cook`) |
| Authoring components, instantiate system, scene view | Asset database (GUIDs, sidecars, import cache), prefabs |
| `ohao/api`: commands with inverses, query, snapshot/restore | Semantic diff (`ohao diff` by entity ID), optional thin Python client |
| `ohao` CLI: `schema`, `validate`, `fmt`, `apply`, `query`, `summary`, `run` | Scripting (see "Language per layer"), physics in the ECS |
| `scene_bench` + checked-in budgets; scene goldens; CLI goldens | Texture overrides in `MaterialOverride` |

Converting `ohao_game`, `turntable` and `env_demo` to scene files is not in 3. Only
`cornell_box` and `model_viewer` gain `--save-scene`.

## Why (what the current code does)

- ECS components are plain structs with no metadata
  (`ohao/ecs/components.hpp`). Nothing can enumerate their fields.
- `MeshRenderer` holds `MeshHandle`/`MaterialHandle`, which are slots in the GPU
  `RenderScene`. `MaterialDesc` holds bindless texture indices. Both are runtime-only.
- Nothing records where a mesh came from, and there are three origins:
  - a file through `ModelLoader`;
  - `PrimitiveMeshGenerator::makeSphere/makeCube`;
  - geometry built inline in example code (`cornell_box.cpp:54` `addWall`,
    `model_viewer.cpp:98` `addQuad` / `:116` `addBox`).

  After `spawnModel` the origin is gone.
- `Parent`/`Children` store `entt::entity`, an index plus a version that EnTT recycles,
  so it isn't stable across sessions.
- `CreationOrder` sets draw order and light order, and the goldens depend on it.
- The camera (`VulkanRenderer::getCamera()`) and the environment
  (`setEnvironmentMap`, `setEnvIntensityScale`) live on the renderer, not in the world.
- Every example is its own executable with its own CLI parsing. There's no single tool
  that loads arbitrary content, and no machine-readable output.

## Architecture

```
ohao/reflect   NEW  ohao_reflect   type registry, attributes, schema + JSON Schema export.
                                   Depends on glm + std only (no EnTT, Vulkan, JSON).
ohao/ecs       +    ohao_ecs       EntityId, authoring components, registration of ECS types,
                                   scene codec, snapshot, instantiate, scene view.
ohao/api       NEW  ohao_api       commands, queries, method dispatch (the one API layer).
tools/ohao     NEW  ohao.exe       the agent/automation CLI over ohao_api.
tools/bench    NEW  scene_bench    benchmarks + budget gate.
```

**Dependency rule, enforced by a test:** nothing under `ohao/render`, `ohao/gpu` or an
ECS *system* source (`transform_system.cpp`, `render_extract.cpp`, `instantiate.cpp`)
includes `reflect/` or `api/`. Reflection serves load, save, validation, commands and
the editor. Simulation and rendering never go through it.

### Type registry (`ohao/reflect`)

- `TypeRegistry` is an explicit object populated by registration functions. There is no
  global and no static-initialization magic.
- Supported field kinds:
  - scalars and strings: `bool`, `int32_t`, `uint32_t`, `uint64_t`, `float`,
    `std::string`;
  - glm: `vec2`, `vec3`, `vec4`, `quat`, `mat4` (16 floats, column-major; used by
    runtime `WorldTransform`, so agents can query world matrices);
  - registered enums (written by name) and nested registered structs;
  - containers: `std::vector<T>`, `std::optional<T>`, `std::variant` of
    registered structs;
  - references: `AssetRef`, and `EntityRef` (an `entt::entity` field, written as the
    target's `EntityId`).
- **Typed codegen at registration, not per-field dispatch.** `.field(name, &T::m)` is a
  template. It instantiates typed encode, decode, compare and copy functions for that
  member, stored as function pointers. Each type also gets typed whole-object
  operations: copy, equality, trivially-copyable flag, and default value. Encoding a
  field is one indirect call into code that knows the exact C++ type; there is no
  switch over field kinds.
- **Attributes:**
  - `.doc("…")` is required for authoring types and fields; a test checks it.
  - `.unit("m" | "deg" | "linear-rgb" | "lux" | …)`.
  - `.min(v)` and `.max(v)` are *hard* limits, enforced by validation and commands.
  - `.uiRange(a, b)` and `.step(s)` are editor slider hints, not enforced.
  - `.color()`, `.angle()`, `.hidden()`, `.readOnly()`.
  - `.alias("oldName")` is a name accepted on load.
- **Per type:** `version` (starting at 1), `upgrade(from, fn)`, `.singleton()` (at most
  one per world), `.component()` (a top-level block in scene files), `.runtime()` (in
  the catalogue, never serialized), and `.skip("name", &T::m, "reason")` for a member
  deliberately not serialized.
- **Entity handles:** reflect never includes EnTT. A handle type becomes an `EntityRef`
  field through a `reflect::ValueTraits<H>` specialization that the ECS provides.

```cpp
// ohao/ecs/scene_types.cpp (shape, not final code)
void registerSceneTypes(reflect::TypeRegistry& r) {
    r.enumeration<LightType>("LightType").doc("Light shape")
        .value("Point", LightType::Point, "Omnidirectional point light")
        .value("Directional", LightType::Directional, "Sun-like, direction = entity -Z")
        .value("Spot", LightType::Spot, "Cone along entity -Z")
        .value("AreaRect", LightType::AreaRect, "Rectangle spanned by areaEdge1/2");
    r.type<Light>("Light", 1).doc("Light source; position and direction from the transform")
        .field("type", &Light::type).doc("Light shape")
        .field("color", &Light::color).doc("Emitted color").unit("linear-rgb").color().min(0.0f)
        .field("intensity", &Light::intensity).doc("Brightness multiplier").min(0.0f).uiRange(0.0f, 100.0f)
        .field("range", &Light::range).doc("Falloff distance").unit("m").min(0.0f)
        .field("radius", &Light::radius).doc("Emitter radius (soft shadows)").unit("m").min(0.0f)
        .field("innerConeDeg", &Light::innerConeDeg).doc("Spot inner cone").unit("deg").angle().min(0.0f).max(180.0f)
        .field("outerConeDeg", &Light::outerConeDeg).doc("Spot outer cone").unit("deg").angle().min(0.0f).max(180.0f)
        .field("areaEdge1", &Light::areaEdge1).doc("AreaRect edge, local space").unit("m")
        .field("areaEdge2", &Light::areaEdge2).doc("AreaRect edge, local space").unit("m");
}
```

### Schema export

Agents and tools in any language need to know what OHAO's components are without
linking or reading C++. That includes an MCP client, a validator in CI, an external
level tool, or a future editor or binding generator. `ohao schema` writes two files,
both canonical and deterministic:

- **`ohao_schema.json`**, the agent-facing catalogue. It lists every type, saved or
  runtime, marked `"serialized": true|false`. For each type it gives:
  - the version, the doc string, whether it is a singleton, and whether it is
    `alwaysPresent` (on every entity: `Name`, `Transform`; left out of files when
    entirely default, and never removable);
  - its upgrade history, as `readsVersions`: every file version the engine loads (the
    current one plus each older version with a complete upgrade path);
  - per field: name, kind, default, `min`/`max`, `uiRange`, unit, doc and aliases;
  - enum values with their docs.

  A field's `unit` is a name from the catalogue's `units` list, `"1"` when it is
  explicitly dimensionless, or `null`. The API method table and the error-code table
  (see "API layer") join the catalogue in the steps that create them (3 and 5). It
  carries a `schemaHash`: a 64-bit FNV-1a of the canonical catalogue. Agents compare it
  to detect a changed engine.
- **`scene.schema.json`**, a JSON Schema (draft 2020-12) for scene documents (`*.scene.json`).
  It's generated from the same registry, including types, enums, limits and
  required-ness. Any language's validator can reject a malformed scene with no OHAO
  binary. The engine's own validator stays authoritative for cross-entity rules (IDs,
  parents, cycles, singletons) that JSON Schema can't express.

### Components: authoring vs runtime

| Component | Kind | Notes |
|---|---|---|
| `EntityId { uint64_t value; }` | authoring (the entity's key) | Written as the entity's `"id"`, not as a component block. 0 is invalid. |
| `Name`, `Transform`, `Parent`, `Light` | authoring | `Transform::matrix()` is a member function, not a field. `Parent::entity` is an `EntityRef`. |
| `MeshSource`, `MaterialOverride` | authoring | New, see below |
| `Camera`, `Environment` | authoring, singleton | New. A singleton means at most one active `Camera` and at most one `Environment`. |
| `Children` | runtime | Rebuilt from `Parent` in load order |
| `WorldTransform` | runtime | Recomputed by `TransformSystem` |
| `MeshRenderer`, `RenderSlot` | runtime | Built by instantiate and extract |
| `EmissiveProxyLight`, `GiMaterialLegacy` | runtime | Derived by instantiate |
| `CreationOrder` | runtime | Reassigned in file order on load; captured exactly by snapshots |
| `PreservedComponents` | runtime | Unknown components from a file, kept opaque |
| `TransformDirty`, `RenderDirty`, `MovedLastFrame`, `InstantiateDirty`, `InstantiateError` | runtime | Tags/diagnostics |

New authoring components:

```cpp
struct AssetRef { std::string path; std::string guid; };   // path: project-relative, '/'; guid empty until the asset DB

struct ModelAsset   { AssetRef asset; };
struct SpherePrim   { float radius{0.5f}; int32_t sectors{32}; int32_t stacks{16}; };
struct CubePrim     { float size{1.0f}; };
struct PlatformPrim { float width{2.0f}; float height{0.4f}; float depth{2.0f}; };
struct CylinderPrim { float radius{0.5f}; float height{1.0f}; int32_t segments{16}; };
struct ConePrim     { float radius{0.5f}; float height{1.0f}; int32_t segments{16}; };
// The examples' inline builders, made reusable (vertex color = color, uv 0, tangent (1,0,0,1)).
struct QuadPrim     { glm::vec3 a, b, c, d; glm::vec3 normal; glm::vec3 color{1.0f}; };
struct BoxPrim      { glm::vec3 lo, hi; glm::vec3 color{1.0f}; };

struct MeshSource {
    std::variant<ModelAsset, SpherePrim, CubePrim, PlatformPrim, CylinderPrim,
                 ConePrim, QuadPrim, BoxPrim> source;
};
// uploadModel's `overrideMaterial`, same rules (render/scene/model_upload.hpp). Scalars only in v1.
struct MaterialOverride { glm::vec3 baseColor{0.8f}; float roughness{0.5f}; float metallic{0.0f}; float emissiveStrength{0.0f}; };
// Pose from WorldTransform (view direction = -Z).
struct Camera { float fovDeg{45.0f}; float nearPlane{0.1f}; float farPlane{100.0f}; bool active{true}; };
struct Environment { std::optional<AssetRef> hdri; float intensity{1.0f}; };
```

`QuadPrim` and `BoxPrim` come from two new generator functions, `generateQuad` and
`generateBox`. They are moved verbatim from the example lambdas, so the vertex streams
stay byte-identical.

### Stable entity IDs

- `World::create` assigns `EntityId = splitmix64(seed + counter++)`, regenerating on
  the vanishingly rare collision. The whole ID generator state is two `uint64_t`s:
  - The seed comes from `std::random_device` by default.
  - `World::seedIds(uint64_t)` makes it deterministic.
  - The counter is part of every snapshot, so a restored world creates the same IDs
    again.
- `World::findById(uint64_t) -> entt::entity` is backed by an open-addressing flat map
  (`ankerl::unordered_dense`). It is kept current through EnTT `on_construct`,
  `on_destroy` and `on_update` of `EntityId`, so it holds on every destroy path
  (`World::destroy`, `registry().destroy`, `registry().clear`).
- Commands and files may also supply explicit IDs (valid, unique) through
  `World::createWithId(id, name, transform)`, which returns `entt::null` for 0 or an id
  in use. Generated ids skip ids in use, so explicit and generated ids never collide.
  Agent-authored content can therefore be fully deterministic.
- `World::idGenState()` / `setIdGenState()` expose the `(seed, counter)` pair that
  snapshots capture. `EntityId` itself is not a registered component: it is the entity's
  key, written as `"id"`.
- An `EntityRef` is written as the target's ID, or `null` if the target is dead. On
  load it's resolved after all entities exist, so file order doesn't matter.

### Instantiate system and scene view

```cpp
class AssetResolver {                                    // ohao/ecs/asset_resolver.hpp
public:
    virtual ~AssetResolver() = default;
    virtual Result<std::shared_ptr<const Model>> loadModel(const AssetRef&) = 0;
    virtual bool exists(const AssetRef&) const = 0;      // validate --check-assets, no load
    virtual std::filesystem::path absolutePath(const AssetRef&) const = 0;
};
class FileAssetResolver final : public AssetResolver { /* project root; caches models by normalized path */ };

// "instantiate" runs FIRST (before "transform"). Requires installRenderExtract.
void installInstantiate(World&, render::RenderScene&, BindlessTextureManager*, AssetResolver&);
[[nodiscard]] std::vector<entt::entity> instantiateErrors(const World&);

[[nodiscard]] SceneView resolveSceneView(const World&);  // active camera params (pitch/yaw from -Z), environment
```

- **Trigger:** `MeshSource`/`MaterialOverride` construct, update or replace tags the
  entity `InstantiateDirty`.
- **Work:** the system builds the `Model` (from the resolver or the generator), then
  calls `ecs::uploadModel` with the override, then emplaces or replaces `MeshRenderer`,
  `GiMaterialLegacy` and `EmissiveProxyLight`. This is the same data path as
  `spawnModel`.
- **Caching:** meshes are cached per distinct `MeshSource` value, with a reference count
  released when the last user goes. Materials are registered per entity.
- **Failure:** `InstantiateError{code, message}`, logged once.
- **Scene view:** a host helper (`tools/ohao/scene_host.hpp`, shared with the two
  examples) applies the `SceneView` to `VulkanRenderer` through its existing camera and
  environment setters. `ohao_ecs` gains no dependency on the renderer camera.
- `spawnModel` stays for in-memory models. Such an entity saves without a mesh, and the
  save report warns `W_UNSAVEABLE_MESH` with its ID.

## Scene file format

### File naming

Scene files are named `<name>.scene.json`. The reasons:

- **Free tooling:** every editor, diff viewer, `jq` and JSON Schema validator recognizes
  `.json` with no configuration. Editors bind `scene.schema.json` to the glob
  `**/*.scene.json`, which gives completion and validation while typing. A two-part
  suffix is the established convention (`package.json`, `*.schema.json`,
  `tsconfig.json`), and agents read the name as "JSON, a scene".
- **Content decides, not the name:** the root member `"format": "ohao.scene"` is
  authoritative. The extension is a hint for tools and people.
- **One format, two renderings:** compact and pretty are the same format, so they share
  the extension.
- **Cooked binary (later):** the cooked form is `<name>.scene.bin`, starting with a magic
  header. A future cooked loader can tell the two apart from the first bytes.
- **Interchange:** OpenUSD import and export (roadmap sub-project 8) keep USD's own
  extensions (`.usda`, `.usdc`, `.usd`). USD is the interchange format for robotics
  and world-model pipelines; `.scene.json` is the native authoring format.
- **Rejected:** YAML is slower to parse, has implicit typing pitfalls (`no`, `1e3`,
  indentation), and gives agents more ways to write an invalid file. A bespoke
  extension (`.ohscene`, the first choice) gets no tooling at all.

### Layout and encoding

- UTF-8, LF line endings, a trailing newline.
- **Canonical:** one world gives exactly one byte sequence.
  - Entities are ordered by `CreationOrder`.
  - Within an entity: `"id"` first, then known components in registration order, then
    preserved unknown components by name.
  - Fields are in registration order. **A top-level field equal to its registered default
    is left out**, and so is an always-present component (`Name`, `Transform`) that is
    entirely default. A fully default optional component is still written, as `{}`
    (its presence is the information). Nested values (inside arrays, optionals and
    variants) are written in full. Equality is bit-exact, so `-0` is written where the
    default is `0`.
  - Why: a missing field decodes to exactly that default, so nothing is lost; diffs show
    only what an author changed; files and query answers shrink. A typical light goes
    from 9 fields to 2 or 3. The scene_tests 10k bench (random positions and rotations, so
    little is default) goes from 2.89 MB to 2.58 MB pretty; authored scenes, where most
    fields stay default, shrink much more.
  - It stays canonical: omission is decided per value by the registered default, so one
    world still gives one byte sequence, and save → load → save is byte-identical.
    `SaveOptions::includeDefaults` (and `scene.save {defaults: true}`) writes every
    field; it loads to the same world and re-saves to the canonical form.
  - **Hazard:** changing a registered default changes the meaning of every file that
    omitted that field. A default change is therefore a component version bump whose
    upgrade writes the old default explicitly into old files. The v1 corpus asserts
    some defaults and catches an accidental change.
- **Two canonical renderings of the same document:**
  - *pretty* is for files: 2-space indent, and objects and arrays print on one line when
    they fit in 100 columns.
  - *compact* is for the wire and for queries: no whitespace.

  Both are deterministic; `ohao fmt` converts between them.
- **Floats** are written with `std::to_chars(float)` (shortest round-trip; `-0`
  preserved). They're read from their raw source token with `std::from_chars<float>`,
  never through `double`, so every float round-trips bit-exactly. NaN and infinities
  are rejected.
- **Encodings:**
  - `vec*` are arrays.
  - `quat` is `[x, y, z, w]` (glTF order).
  - Enums are names.
  - An empty optional is `null`.
  - A variant is a one-key object `{ "TypeName": { … } }`.
  - IDs and `uint64_t` are 16-digit lowercase hex strings.
  - `AssetRef` is `{ "path": … }`, plus `"guid"` only when it's non-empty.
- **On read:** a missing field takes the struct default, and a missing `Name`/`Transform`
  takes the default component. Agents may write minimal files in any member order;
  `ohao fmt` puts them in canonical form.
- **Header:** `format: "ohao.scene"`, the document `version` (1), and `components`
  (each type present → the version it was written with, keys sorted). The file contains
  no timestamps and no machine-specific data.

Example (excerpt of `assets/scenes/cornell_box.scene.json`; the real file is generated by
`cornell_box --save-scene`):

```json
{
  "format": "ohao.scene",
  "version": 1,
  "components": {
    "Camera": 1,
    "Environment": 1,
    "Light": 1,
    "MaterialOverride": 1,
    "MeshSource": 1,
    "Name": 1,
    "Parent": 1,
    "Transform": 1
  },
  "entities": [
    {
      "id": "3f9a61c2d8e04b17",
      "Name": { "value": "MainCamera" },
      "Transform": { "position": [0, 0, 13] },
      "Camera": { "fovDeg": 38 }
    },
    { "id": "a1c47e09b3f25d68", "Name": { "value": "Environment" }, "Environment": {} },
    {
      "id": "5e20b8d4c19f7a03",
      "Name": { "value": "Back" },
      "MeshSource": {
        "source": {
          "QuadPrim": {
            "a": [-5, -5, -5], "b": [5, -5, -5], "c": [5, 5, -5], "d": [-5, 5, -5],
            "normal": [0, 0, 1], "color": [0.73, 0.73, 0.73]
          }
        }
      },
      "MaterialOverride": { "baseColor": [0.73, 0.73, 0.73], "roughness": 0.95 }
    },
    {
      "id": "c8d2f61a4b7e9035",
      "Name": { "value": "MetalSphere" },
      "Transform": { "position": [-2, -3, 0], "scale": [2, 2, 2] },
      "MeshSource": { "source": { "SpherePrim": { "radius": 0.5, "sectors": 32, "stacks": 16 } } },
      "MaterialOverride": { "baseColor": [0.95, 0.93, 0.88], "roughness": 0.05, "metallic": 1 }
    },
    {
      "id": "07b9e3a5d61c4f82",
      "Name": { "value": "Light0" },
      "Transform": { "position": [-3, 4, -3] },
      "Light": { "color": [1, 0.3, 0.2], "intensity": 5, "radius": 0.3 }
    },
    {
      "id": "e46a0f2c9d8b1357",
      "Name": { "value": "LampShade" },
      "Transform": { "position": [0, 0.4, 0] },
      "Parent": { "entity": "07b9e3a5d61c4f82" },
      "MeshSource": { "source": { "ModelAsset": { "asset": { "path": "assets/props/lamp_shade.glb" } } } }
    }
  ]
}
```

(The last entity shows a parented model reference; it isn't part of the real Cornell
file.)

### Loading and validation

- **Two-phase.**
  1. Parse (simdjson On-Demand, in `ohao_ecs`) into a small read-only JSON tree
     (`reflect::JsonDocument`, which keeps each number's raw token), then decode the
     whole document into typed per-component values. All errors are collected here, up
     to 100 in document order, then `E_TOO_MANY_ERRORS`.
     - A per-thread `JsonParser` keeps the simdjson parser, the padded input and the
       tree storage warm, so repeated loads allocate nothing once warm.
     - Floats are parsed from their tokens with fast_float, which is correctly rounded
       and gives `std::from_chars<float>` results bit for bit, faster. Overflow and
       underflow are `E_RANGE`.
  2. Only if phase 1 reports no errors, create entities in file order and emplace
     components.

  The destination world is never partially modified. Emplacing fires the normal
  signals, so dirty tagging, instantiate and extract need no special load path.
  `Parent` is applied after all entities exist, through `World::setParent`.
- **Unknown components** are kept in `PreservedComponents` as canonical compact JSON
  text and written back on save, with the version from the file header. An unknown
  *field* inside a known component is an error (`E_UNKNOWN_FIELD`) with a suggestion.
  Agent typos must not vanish silently:
  - An unknown component whose name is one edit (or one swap of adjacent letters) from
    a known component draws `W_UNKNOWN_COMPONENT` with the suggestion.
  - A registered runtime-only component in a file (for example `WorldTransform`) is
    `E_UNKNOWN_FIELD`.
  - A header version that differs from the engine's is `E_COMPONENT_VERSION`, and that
    component is not decoded (step 4 adds the upgrades that make older versions load).
- **Error object.** It's the same in files, commands, CLI output and the future RPC:

```json
{
  "code": "E_UNKNOWN_FIELD",
  "path": "/entities/4/Light/intensty",
  "entity": "07b9e3a5d61c4f82",
  "line": 61, "column": 9,
  "message": "Light has no field 'intensty'",
  "expected": "one of: type, color, intensity, range, radius, innerConeDeg, outerConeDeg, areaEdge1, areaEdge2",
  "got": "\"intensty\"",
  "suggestion": "intensity"
}
```

  Error codes are stable strings, listed in the schema catalogue:

  | Group | Codes |
  |---|---|
  | Files | `E_IO` |
  | Parsing and document | `E_PARSE`, `E_FORMAT`, `E_VERSION_UNSUPPORTED`, `E_COMPONENT_VERSION`, `E_UPGRADE`, `E_WORLD_NOT_EMPTY` |
  | Values | `E_UNKNOWN_FIELD`, `E_DUPLICATE_KEY`, `E_TYPE`, `E_ENUM`, `E_RANGE`, `E_NONFINITE` |
  | References | `E_ENTITY_NOT_FOUND` (an entity reference other than `Parent`) |
  | Warnings | `W_UNKNOWN_COMPONENT` |
  | IDs and hierarchy | `E_ID_INVALID`, `E_ID_DUPLICATE`, `E_PARENT_MISSING`, `E_PARENT_CYCLE`, `E_SINGLETON` |
  | Assets | `E_ASSET_NOT_FOUND` (with `--check-assets`) |
  | API and commands | `E_METHOD`, `E_COMMAND`, `E_COMPONENT_UNKNOWN`, `E_PATH` (and `E_ENTITY_NOT_FOUND` for command targets) |
  | Limits | `E_TOO_MANY_ERRORS` |

  Warnings use the same shape with `W_` codes. `line`/`column` are present for file
  input. `path` is always present (a JSON Pointer into the document or into the command
  batch).

### Versioning and migration

- **The document `version`** changes only when the envelope changes. A loader accepts
  every version it knows and rejects newer ones with `E_VERSION_UNSUPPORTED`.
- **Each component has its own version.** Registering version N requires upgrade
  functions `1→2 … (N-1)→N`, operating on a small internal JSON tree before decoding.
  The load report lists every upgrade applied.
  - Registration is `.upgrade(from, fn)` with
    `reflect::Upgrade = std::function<bool(JsonValue&, std::string& error)>`.
    `JsonValue` is reflect's small owning, mutable JSON value: `find`, `set`, `erase`,
    `rename` (position kept), `asFloat`/`asInt`, and canonical number constructors.
  - `lint()` reports every missing step, so a version bump without its migration fails
    the registration tests.
  - **On load**, the file header's version decides, per component type:
    - the current version is decoded directly; current files never touch the upgrade
      code;
    - an older version with a complete path has each block copied into a `JsonValue`
      and upgraded step by step. The result goes through the ordinary typed decoder on
      a side document. Its errors keep their JSON Pointer path, point at the original
      block's line, and say "after upgrading …";
    - a newer version, or an older one with no path, is `E_COMPONENT_VERSION`, whose
      `expected` lists the readable versions.
  - An upgrade that rejects the data is `E_UPGRADE`, with its reason.
  - All of this is phase 1, so a failed or rejected migration leaves the world
    untouched.
  - `LoadReport::upgraded` and `ValidationReport::upgraded` list
    `{component, from, to, count}` once per component type.
- **A rename** is an `alias`, and adding a defaulted field needs no bump. **Removing or
  retyping** a field does need a bump.
- **Migration corpus:** `tests/scenes/v1/*.scene.json` is never regenerated, and every
  future build must load it with the values the test asserts:
  - `rooms.scene.json`: canonical, a hierarchy of every saved v1 component;
  - `agent_minimal.scene.json`: minimal, with no header, defaults and free member order;
  - `preserved.scene.json`: an unknown component with its header version.

## API layer (`ohao/api`) — the one API for humans and agents

Every authoring operation is a method with JSON-encodable parameters and results. The
CLI, the future `ohao serve`, the future C ABI and the future editor all dispatch
through this table, so there is exactly one implementation of each operation.

`api::Session(World&, const TypeRegistry&)` owns the dispatch:
`std::string call(std::string_view method, std::string_view params)`.
- `params` is a JSON object (empty means `{}`). The reply is compact JSON, exactly one of
  `{"result": …}` or `{"error": {"code", "message", "data": [diagnostics]}}`.
- `code` and `message` are the first diagnostic's (with "(+N more)"); `data` holds up to
  20 diagnostics in the file-validation shape, with paths into the params (for example
  `/commands/3/value`). The JSON-RPC integer error codes are mapped in sub-project 4.
- Optional members are left out of results when empty, and unknown params are
  `E_UNKNOWN_FIELD` with a suggestion.
- The method table (name, params, result, doc) is part of the catalogue
  (`"methods"`) and of `schemaHash`.

| Method | Params → result |
|---|---|
| `schema.get` | → the catalogue (as `ohao schema`, compact), with `methods` and `schemaHash` |
| `scene.validate` | `{text \| path \| scene, limit = 20}` → `{ok, errors?, warnings?, upgraded?, more?}` |
| `scene.load` | `{text \| path \| scene, seed?}` → `{entities, preserved?, upgraded?, warnings?}`. The world must be empty; all or nothing. |
| `scene.save` | `{path?, layout = compact \| pretty, defaults = false}` → `{scene}` (the compact scene as an object) or `{path, bytes}` |
| `world.info` | → `{entities, frame, schemaHash, idCounter}` |
| `world.summary` | `{depth = 2, maxChildren = 8, maxNodes = 64}` → `{entities, roots, components, bounds?, outline, moreRoots?}` |
| `world.query` | `{ids?, names?, nameContains?, has?, fields?, limit = 50, offset = 0, defaults = false}` → `{entities, count, missing?, next?}` |
| `world.apply` | `{commands}` (one atomic batch) → `{applied, results, inverse}` |
| `world.step` | `{frames = 1, dt = 1/60}` → `{frame}` |
| `world.snapshot` | `{name}` → `{name, entities, bytes}` (step 6) |
| `world.restore` | `{name}` → `{created, destroyed, changed}` (step 6) |
| `render.capture` | `{path, mode, spp, width, height, denoise}` → `{path, ms}`. The renderer is initialized lazily on first use. (step 9) |

- `scene` is the scene as a JSON object: it costs no string escaping, which matters
  when an agent writes it. `text` is the same as a string, `path` a file. `seed` (16
  hex digits) fixes the id generator for later creates. `checkAssets` arrives with
  assets (step 7).
- **`world.query`:**
  - Filters all have to match: `ids` (any of), `names` (exact, any of), `nameContains`
    (substring), `has` (every listed component, runtime ones included).
  - Entities come in creation order, `limit`/`offset` page them, `count` is the total
    match count and `next` the following offset. Ids that do not exist are listed in
    `missing`, not an error.
  - **Projection:** without `fields`, each entity is its compact file form (defaults
    omitted unless `defaults: true`). `fields: ["/Light", "/Transform/position"]` gives
    whole components or single fields (always written, even when default). Runtime
    components can be projected too (`/WorldTransform/world`). `fields: []` gives ids
    only.
- **`world.summary`:**
  - Counts per component (runtime included) and the number of roots.
  - `bounds` of the entity origins in world space, composed from local transforms, so
    they are current before any step.
  - An `outline` of the hierarchy in creation order, `depth` levels below the roots.
    Each node is `{id, name?, children?, more?}`, where `more` counts the direct children
    not listed (depth, `maxChildren` or `maxNodes` reached).

**Commands** (the editor and agents both mutate authoring state only through these;
gameplay systems mutate simulation state, and snapshots capture both):

```json
{ "op": "entity.create", "id": "optional-hex", "name": "Lamp", "parent": "07b9e3a5d61c4f82",
  "components": { "Light": { "type": "Spot", "intensity": 8 } } }
{ "op": "entity.destroy", "id": "…", "recursive": true }
{ "op": "entity.reparent", "id": "…", "parent": "…" }
{ "op": "component.set", "id": "…", "type": "Light", "value": { … } }
{ "op": "component.remove", "id": "…", "type": "Light" }
{ "op": "field.set", "id": "…", "path": "/Light/intensity", "value": 12.5 }
```

- **Atomic batches:**
  - Commands apply in order. Each is validated against the world as the earlier ones
    left it (with the same typed decoders, limits and reference checks as files), so a
    batch can create an entity and then edit it.
  - A command checks everything before it changes anything. Changes go through
    `emplace_or_replace`, `remove` and `World::setParent`, so the usual signals fire.
  - When a command fails, the commands already applied are undone by running their
    inverses in reverse order, and the id generator and creation sequence are
    restored. All or nothing: the world saves byte-identically to before.
  - *Revised in step 5:* the first design validated the whole batch before applying
    any of it. That cannot check a command against entities an earlier command of the
    same batch creates, reparents or destroys, so validation moved into the sequential
    apply, with rollback.
- **Command details:**
  - `entity.create`: `components` accepts every saved component, `Name`, `Transform`
    and `Parent` included; giving both `name` and `Name` (or `parent` and `Parent`) is
    `E_DUPLICATE_KEY`. Unknown components are `E_COMPONENT_UNKNOWN` (commands do not
    preserve typos). The restore-only members `order` (the `CreationOrder`) and
    `preserved` (`{Name: {version, value}}`) let inverses bring back an entity exactly.
  - `entity.destroy`: recursive by default. With `recursive: false` the children are
    detached (they become roots) and survive.
  - `Parent` changes always go through `World::setParent`, whichever command makes them
    (`entity.reparent`, `component.set`/`component.remove` of `Parent`, or `field.set`
    of `/Parent/entity`). Cycles are `E_PARENT_CYCLE`.
  - `Name` and `Transform` are on every entity and cannot be removed (`E_COMMAND`).
  - `field.set` paths are `/Type/field`, optionally followed by member names or array
    indices (`/Transform/position/1`). A deep path re-decodes the whole field through the
    typed decoder, so limits and types are checked exactly as in files. A field of a
    component the entity lacks is `E_COMMAND` (use `component.set`).
  - Command-level `id` and `parent` also accept `"$k"`: the entity created by command
    `k` of the same batch (an earlier `entity.create`).
- **Results:** `results[i]` is the id that command `i` created, else `null`.
- **Inverses:** every applied batch returns its inverse batch. Undo/redo is replaying
  inverses (the inverse of the inverse redoes), and the editor later coalesces a gizmo
  drag into one `field.set`.
  - create → destroy; destroy → `entity.create` per entity in pre-order (with `order`,
    its file-form components and `preserved`), plus reparents of detached children;
    reparent → reparent back; `component.set` → the old value or `component.remove`;
    `component.remove` → `component.set`; `field.set` → the old value at the same path.
  - A batch's inverse is its commands' inverse lists, last command first.
  - Known limit: an undo that recreates a child appends it to its parent's `Children`,
    so sibling order can differ after undo. Saves are unaffected (they order by
    `CreationOrder`, which is restored); step 6's restore does not have this limit.
- **One semantics for create:** `entity.create` with components is the same thing as
  loading that entity from a file. A test proves the command-built world saves to the
  same bytes.
- **New error codes:** `E_METHOD` (unknown method), `E_COMMAND` (malformed request or
  an edit that cannot apply), `E_COMPONENT_UNKNOWN` (not a component, or not a saved
  one for edits), `E_PATH` (a path that reaches no field).

### Token budget (agents)

Agents pay for every byte they read and write, so token efficiency is a requirement,
not a nicety. **An agent never needs to read a whole large scene.** It orients with
`world.info`/`world.summary`, reads exactly what it needs with filtered, projected
`world.query`, edits with `field.set`, and reads terse errors. Concretely:

1. **Defaults are omitted** from canonical files and from query answers (see "Layout
   and encoding"); `defaults: true` asks for everything.
2. **Filters and projection** in `world.query` (and the CLI later) keep answers to the
   entities and fields asked for.
3. **`world.summary`** gives counts, bounds and a capped outline in a fixed small size,
   whatever the scene size.
4. **Errors are terse:** one diagnostic is one line of JSON with code, path, expected,
   got and a suggestion; "one of" lists are cut after 12 names ("…, +N more"); an error
   reply carries at most 20 diagnostics, and `scene.validate` returns at most `limit`
   (20) of each kind with a `more` count.

Estimates, measured on the 10k-entity test world (`api_tests`, tokens ≈ bytes / 4):

| Operation | Request + reply | ≈ tokens |
|---|---|---|
| `world.query` one entity, one projected field | 61 + 98 B | 40 |
| `world.apply` one `field.set` (reply includes its inverse) | 100 + 137 B | 60 |
| One error (e.g. a misspelled field, with suggestion) | 289 B | 70 |
| `world.info` | ~90 B | 25 |
| `world.summary` of 10k entities (64 outline nodes) | 3.3 KB | 830 |
| `world.query` 1,000 entities, full components | 122 KB | 31k |
| The whole 10k scene, compact (never needed) | 1.26 MB | 310k |

A typical edit loop (summary once, then query + edit + verify per change) costs about
1k tokens up front and 100–200 tokens per edit, independent of scene size.

**`ohao` CLI** (`tools/ohao`, built on `ohao_api`):

```
ohao schema   [-o dir]                       # ohao_schema.json + scene.schema.json
ohao validate <scene> [--check-assets]
ohao fmt      <scene> [--check] [--compact] [-o out]
ohao apply    <scene> <commands.json> [-o out]
ohao query    <scene> [--ids a,b] [--name N] [--name-contains S] [--has T,U] [--fields /T,/T/f]
              [--limit N] [--offset N] [--defaults]            # = world.query
ohao summary  <scene> [--depth D] [--max-children N] [--max-nodes N]   # = world.summary
ohao run      <scene> [--seed S] [--steps N] [--dt S] [--script ops.jsonl]
              [--render out.png --mode deferred|rt_realtime|rt_offline --spp N --denoise X --size WxH]
              [--save out.scene.json]
```

- **Output:** each invocation writes one JSON document to stdout. For `--script`, it
  writes one JSON line per operation: `{"method": …, "result": …}` or
  `{"method": …, "error": {…}}`. Human logs go to stderr.
- **Exit codes:**
  - 0 = ok;
  - 1 = validation or usage error;
  - 2 = asset or instantiate error;
  - 3 = GPU or internal error.
- **Script lines:** each one is `{"method": "...", "params": {...}}`, a JSON-RPC 2.0
  request minus `jsonrpc`/`id`, so a script replays verbatim over the future RPC
  transport. Example:

```
{"method":"world.snapshot","params":{"name":"base"}}
{"method":"world.apply","params":{"commands":[{"op":"field.set","id":"c8d2f61a4b7e9035","path":"/Transform/position/0","value":-1}]}}
{"method":"world.step","params":{"frames":10}}
{"method":"world.query","params":{"ids":["c8d2f61a4b7e9035"],"fields":["/Transform"]}}
{"method":"render.capture","params":{"path":"moved.png","mode":"deferred"}}
{"method":"world.restore","params":{"name":"base"}}
```

- **No GPU unless needed:** without a `render.capture` (or `--render`), no Vulkan
  device is created. Load, validate, query, apply, step, snapshot and save run CPU-only.
- **Determinism contract:**
  - After any sequence of `scene.load`, `world.apply`, `world.step`, `world.snapshot`,
    `world.restore` and `scene.save`, the world state and stdout are bit-identical
    across runs of the same build, given the same inputs and `--seed`.
  - Systems may not read the wall clock or unseeded randomness. The fixed `dt` is the
    only time source.
  - Pixels are deterministic only where the goldens say so:
    - `cornell_box` RT offline and deferred are exact;
    - `damaged_helmet_deferred` and `rt_realtime` have known run-to-run noise;
    - agents get the golden tolerances.

**Future transport (sub-project 4, not in 3), designed now:**
- **`ohao serve --stdio`:** a JSON-RPC 2.0 server with LSP-style `Content-Length`
  framing, the method table above, and a `protocolVersion` and `schemaHash` handshake.
  MCP clients connect natively: each method is exposed as an MCP tool, and its params
  schema comes from the catalogue.
- **Binary payloads** (PNG renders, bulk observations) go by file path in v1. Shared
  memory comes later.
- **The C ABI** wraps the same dispatch:
  - `ohao_status ohao_call(ohao_ctx*, const char* method, const char* params, size_t len, ohao_buf* out)`;
  - later, zero-copy typed views (for example a transform column) for agents that step
    thousands of times per second and can't afford JSON per frame.

## Performance

Performance is a gate, not a follow-up. There are four rules.

1. **Reflection costs nothing per frame.** Systems use EnTT views over plain structs.
   The registry is touched only by load, save, validate, commands, schema, and the
   editor later. The dependency rule above is tested.
2. **Typed inner loops.** Encode, decode, copy and compare go through per-member
   function pointers generated from templates at registration. There's no kind-switch
   per value and no `std::any`.
3. **The agent's hot loop never touches text.** Step, query, commands, snapshot and
   restore work in memory. Text is for authoring, interchange and git.
4. **Snapshots are columnar and memcpy-friendly.**
   - **Capture:** a snapshot stores the entity table (`EntityId`, `CreationOrder`, the
     ID-generator counter, the frame counter) plus one column per registered component
     type, holding row indices and values.
     - Trivially copyable components (`Transform`, `Light`, `Camera`,
       `MaterialOverride`) are captured with one `memcpy` of the EnTT packed storage.
     - `Parent` is stored as row indices.
     - Non-trivial components (`Name`, `MeshSource`, `Environment`,
       `PreservedComponents`) are captured by typed copy.
   - **Restore** diffs instead of rebuilding:
     - ID sets are merge-compared.
     - Only missing entities are created (with their saved `CreationOrder`, so draw and
       light order are exact) and only extra ones destroyed.
     - Values are compared per row (`memcmp` or typed `==`), and only rows that differ
       are written through `replace`.

     Downstream systems therefore see only real changes: a restore of an unchanged
     world uploads **zero** instance bytes to the GPU.
   - **Cooked format later:** the column layout *is* the future cooked binary layout
     (little-endian, `schemaHash`-stamped, string table for non-trivial columns).
     `ohao cook` will be "load text → snapshot → write columns", and a cooked load will
     be `mmap` + `memcpy` per POD column.

### Budgets (gated)

Measured by `scene_bench` (Release) on the reference machine, Frank's dev laptop
(confirmed 2026-10-22):
- median of 50 iterations after 5 warm-ups, p95 also reported;
- single thread, no GPU.

Scene `bench10k` is generated with a fixed seed:
- 10,000 entities: 9,900 renderables (`Name`, `Transform`, `MeshSource` primitive,
  `MaterialOverride`) and 100 lights;
- 1 camera and 1 environment;
- hierarchy depth up to 8;
- about 5 MB pretty.

| Operation (bench10k) | Budget (median) |
|---|---|
| `saveScene` pretty, World → string | ≤ 10 ms |
| `loadScene`, string → empty World (both phases) | ≤ 20 ms |
| `scene.validate`, string → errors, no World | ≤ 10 ms |
| `snapshot` | ≤ 1 ms |
| `restore`, nothing changed | ≤ 1 ms |
| `restore`, 1% of rows changed | ≤ 2 ms |
| `restore` into an empty world (everything recreated) | ≤ 10 ms |
| `world.apply`, batch of 100 `field.set` | ≤ 0.2 ms |
| `world.query`, 1,000 IDs, full components, compact | ≤ 2 ms |

- **Scaling:** `bench100k` must stay within 12× the 10k numbers (near-linear).
- **Recorded, not gated:**
  - process wall time of `ohao validate` and `ohao run --steps 1` on bench10k;
  - load-to-first-frame of the Cornell scene with rendering.
- **Gate mechanics:** `scene_bench --check tests/bench/scene_budgets.json` exits
  nonzero on any regression and writes `scene_bench.json`. Changing a budget is a
  ledgered ruling that needs Frank's OK.

### Tension: canonical/readable vs fast

Canonical text costs CPU:
- sorting by creation order;
- shortest-float formatting;
- comparing every field with its default (to leave defaults out);
- line-fitting pretty-printing.

It also costs size: about 5 MB for 10k entities. Readable formats are inherently slower
than binary. That cost buys diffs, agent readability and determinism. The resolution is
**three tiers, each used where it wins:**

1. **Text** (`.scene.json`, pretty or compact) for authoring, review, git and agents.
   It's fast enough to sit under a budget (simdjson at GB/s, `to_chars`, a hand-written
   writer into one reserved buffer, no iostreams), and it never sits on a per-step
   path.
2. **In-memory columnar snapshots plus commands** for every per-step agent operation,
   at memcpy speed.
3. **Cooked binary** (later) for shipping and very large levels, which share the
   snapshot layout.

The pretty/compact split removes the formatting cost from machine-to-machine traffic
without a second format.

## Language per layer

Rule: efficiency first. Compiled, low-overhead languages for everything in the engine,
its tools and its control surface. No TypeScript, and no GC-heavy or interpreted
runtime on any hot path. Language-neutral boundaries mean another language can be
adopted later for a whole layer without touching the core.

| Layer | Language / tech | Why |
|---|---|---|
| Core runtime, ECS, renderer, physics glue | **C++20 (keep)** | It's 54K working lines. Vulkan, Jolt, NRD, DLSS, OIDN, assimp/ufbx and EnTT are C/C++ APIs. A rewrite costs months and buys no measurable speed. |
| Reflection + schema | **C++20, explicit registration** | It must see the C++ component types at zero runtime cost. Its output (JSON catalogue, JSON Schema) is language-neutral. |
| Scene codec, validator, migrations | **C++20 + simdjson + `to_chars`** | One implementation of the format, parsing at GB/s. A second implementation in another language would drift. Others validate through the JSON Schema. |
| API layer (`ohao/api`) | **C++20** | In-process with the World: no FFI, no copies. |
| Agent CLI (`ohao`) | **C++20** | Same binary as the engine; no runtime start-up tax. |
| Control surface (later) | **C++20, JSON-RPC 2.0 over stdio, named pipe later** | MCP *is* JSON-RPC over stdio, so a native server needs no Node or Python hop. |
| In-process embedding (later) | **C ABI (`ohao_c.h`)** | The one ABI Rust, Zig, Python and others can all call. |
| Bulk observations for high-rate agents (later) | **C ABI zero-copy views / shared memory** | JSON per step is too slow at thousands of steps per second. |
| Agent client bindings (optional, later) | **None needed for MCP agents.** If ever wanted, a thin stdlib-only Python wrapper over the protocol. | Convenience only. It's a wrapper over the native protocol, never on a hot path. No TypeScript. |
| Editor UI (later) | **C++20 + Dear ImGui (docking), Vulkan backend** | Same process as the engine, no second UI runtime, fits reflection-driven inspectors. It's a client of `ohao/api` like any agent. |
| Gameplay logic (later; proposed direction, not a commitment) | **Native C++ systems, hot-reloaded as DLLs** | Full speed with direct ECS access. |
| Sandboxed scripting for agent- or mod-authored code (later; proposed direction, not a commitment) | **WebAssembly behind an AOT-capable C runtime** (choice made in that spec) | A sandbox for untrusted code, any source language, near-native speed with AOT. |
| Shaders | **GLSL (existing); Slang for new shaders from sub-project 2b on** | Both compile to SPIR-V (same GPU speed). Slang adds modules, generics and interfaces that cut copy-pasted includes, plus D3D12/Metal backends if ever needed. Existing GLSL migrates only when rewritten. |
| Asset pipeline / cooker (later) | **C++20** | Reuses ModelLoader, assimp/ufbx and the C/C++ texture compressors. |
| Build | **CMake + FetchContent (keep)** | It already works with the VS 2026 generator. |
| Benchmarks, unit and engine tests | **C++ executables** | The measurements are taken in the language being measured. |
| Golden, validation and monograph harnesses | **Python (keep, test-only)** | Already written, off every hot path, never shipped. No new Python in engine or tool paths. |

**On rewriting the core:** keep it. The C++/Vulkan core is the most expensive and best
verified part of OHAO. It has goldens, validation-clean runs and two reviewed
sub-projects of fixes. Porting it to Rust would mean re-binding Jolt, NRD, DLSS and
OIDN, re-proving every golden, and losing months, all for safety benefits the existing
tests already approximate. Rust or Zig become sensible only for a *standalone,
out-of-process* tool behind one of the boundaries below, and only if it needs a library
C++ lacks. No second compiled language is added in 3.

**Language-neutral boundaries:**
1. The scene format: `.scene.json` plus `scene.schema.json`.
2. The catalogue: `ohao_schema.json` with its `schemaHash`.
3. The protocol: the method table, the command objects and the error objects above,
   with `protocolVersion: 1`.
4. The C ABI (later).
5. The cooked binary formats (later; little-endian, versioned, hash-stamped).

## Engine development by agents (G2)

Today an agent working on OHAO has to rediscover too much. These traps are real and
each one has cost this project time:
- the `shaders` target is manual, so a green build can carry stale SPIR-V;
- conda's `cmake` shadows the real one;
- test results are free-form text;
- `.claude/skills/` still describes the deleted Godot/scons engine (12 of 14 skill
  files mention it);
- the build gotchas live only in one assistant's private memory, not in the repo.

### In sub-project 3

These are cheap, and they document what 3 itself creates:

1. **`AGENTS.md` at the repo root.** It's the single entry point for any agent, with a
   one-line `CLAUDE.md` that imports it. It covers:
   - the build commands (the scoop `cmake` path, the `shaders` target and when it's
     needed);
   - every test suite and gate, with its pass criterion;
   - the golden and validation harnesses;
   - the gotchas list, moved in-repo;
   - the conventions:
     - spec → plan → build;
     - a `docs/changes/YYYY-MM-DD-<subproject>.md` record per sub-project, with its
       template;
     - the commit and line-ending rules (`core.autocrlf`);
     - forward-only docs.
2. **`docs/ARCHITECTURE.md`, a map of the code.** It lists the libraries and their
   dependency directions, and the frame data flow (game systems → transform →
   instantiate → extract → `RenderScene` → passes). It also holds recipes with exact
   files, each one written and then followed once to prove it:
   - add a component (struct, registration with docs/units/limits, completeness test,
     version bump + upgrade when changing it, corpus case);
   - add a system (where it goes in the order);
   - add an API command;
   - add a test suite.
3. **Refresh `.claude/skills/`.** Delete the stale Godot/scons skills, including
   `_disabled/`, per the forward-only rule. Replace them with current ones:
   - `build`: configure, build, shaders;
   - `verify`: the full gate set with pass criteria;
   - `add-component`: the recipe above.

   Each skill is checked by running every command in it.
4. **Machine-readable output from everything new in 3:**
   - `ohao` (JSON);
   - `scene_bench` (JSON plus budget exit code);
   - the new test suites (a final one-line JSON summary
     `{"suite","passed","failed","failures":[…]}` after the human output).

### In sub-project 4, alongside `ohao serve`

1. **One build-and-test entry point** with a clear pass/fail:
   - CTest registration of every suite, golden and validation run, with labels
     `quick` / `gpu` / `golden` / `bench`;
   - a `check` target that builds the shaders first, closing the stale-SPIR-V trap;
   - JUnit XML via `ctest --output-junit`.

   It's native CMake, so there's no new runtime.
2. **Machine-readable test output:**
   - the shared test-harness macros print the JSON summary for every existing suite;
   - `render_golden.py` and `check_validation.py` gain `--json` and write a diff heatmap
     PNG per failing golden for the agent to inspect.
3. **`ohao doctor`:** checks the environment (Vulkan device, GPU, the cmake on PATH,
   shader freshness against sources, assets present) and reports a JSON verdict.
4. **More recipes in `ARCHITECTURE.md`:** add a render pass, add a shader, add a golden
   scene.
5. **The G1 surface for game development:**
   - `ohao serve` with the method table above;
   - input injection through the runtime's action map;
   - live-window frame capture;
   - pause, step and resume;
   - log streaming.

   Writing gameplay *code* end to end additionally needs the gameplay/hot-reload
   sub-project (see "Language per layer"); until then agents build games from scenes,
   components and engine-provided systems.

## Programs

- **`ohao`** (above) replaces the earlier plan for a separate scene viewer.
- **`cornell_box` and `model_viewer`:**
  - they gain `--save-scene <path>`, which writes their world with fixed IDs
    (`World::seedIds`) and exits without rendering;
  - their inline builders become `QuadPrim`/`BoxPrim` entities;
  - they author their camera and environment as entities and apply them through the
    shared scene-host helper.
- **`scene_bench`** generates the bench scenes, runs the budget gate and dumps JSON.
- **Checked-in scenes:**
  - `assets/scenes/cornell_box.scene.json`;
  - `assets/scenes/damaged_helmet.scene.json`, with the baked `SceneFramer` room, camera
    and lights for `DamagedHelmet.glb`.

## Test plan

CPU tests (`tests/reflect` → `reflect_tests` for the library alone; `tests/scene` →
`scene_tests` for the engine registrations, ids and later the codec, commands and
snapshots; neither creates a GPU device):

1. **Completeness:**
   - for every registered aggregate, registered + skipped fields ==
     `boost::pfr::tuple_size_v<T>`;
   - every authoring type and field has a doc string;
   - every numeric field either has a unit or is marked unitless.
2. **Round trip, canonical:** save → load → save is byte-identical in both pretty and
   compact form. Components compare field-by-field equal.
3. **Seeded random worlds:**
   - 1000 entities;
   - hierarchy depth up to 32, with wide fans;
   - every light type and every `MeshSource` alternative;
   - floats including denormals, `-0`, `FLT_MAX`, `FLT_MIN` and `0.1`;
   - Unicode and escaped names.

   Round trip as in 2.
4. **Hierarchy and order:**
   - `WorldTransform` is bit-identical after one update;
   - creation order, light order and extract instance order are preserved;
   - references that come before their target in file order resolve.
5. **IDs:**
   - seeding makes saves reproducible;
   - IDs survive round trips and restore;
   - `findById` stays current;
   - dangling references are written as `null`;
   - explicit IDs are accepted, and duplicates rejected.
6. **Error corpus:** `tests/scenes/invalid/*.scene.json`, each paired with an
   `*.errors.json` holding the exact expected `code` and `path` list (plus `line` and
   `suggestion` where relevant). It covers every error code. Multiple errors per file
   must all be reported, and the destination world must be unchanged.
7. **Unknown components** survive load → save byte-identically. **Unknown fields** fail
   with a suggestion.
8. **Migration:**
   - a test component at version 2 with an upgrade loads its version-1 file;
   - the `tests/scenes/v1` corpus loads with the expected values.
9. **Commands:**
   - each op applies and validates;
   - a failing command inside a batch leaves the world unchanged;
   - applying a batch and then its inverse gives byte-identical saves;
   - a command-built world saves identically to the file-loaded one;
   - `field.set` outside `min`/`max` fails with `E_RANGE`.
10. **Snapshot/restore:**
    - mutations (move, add, destroy, reparent, rename) followed by restore give saves
      identical to before;
    - the ID counter is restored, so new IDs repeat;
    - an unchanged restore writes no component.
11. **Fake resolver:** instantiate emplaces `MeshRenderer` or `InstantiateError`
    without touching the disk.
12. **Schema:**
    - `ohao_schema.json` is deterministic and its `schemaHash` stable;
    - `scene.schema.json` accepts every checked-in scene and rejects the type and
      range cases of the error corpus. This runs through Python `jsonschema` in a
      test-only script.
13. **Dependency rule:** no `reflect/` or `api/` include under `ohao/render`, `ohao/gpu`
    or the ECS system sources.

CLI and GPU tests:

14. **CLI goldens:**
    - scripted `ohao run` sessions on CPU only (snapshot, apply, step, query, restore)
      have checked-in expected stdout;
    - two runs are byte-identical;
    - each exit code is covered.
15. **Scene goldens:** five manifest entries render `ohao run <scene> --render …` and
    compare against the **existing** golden images with the existing tolerances:
    - `cornell_box` RT: exact;
    - `cornell_box_deferred`: exact;
    - `damaged_helmet` RT: 4 / 0.01;
    - `damaged_helmet_deferred`: 6 / 0.05;
    - `cornell_box_rt_realtime`: 10 / 0.05.
16. **Code-built vs file:**
    - `cornell_box --save-scene` and `model_viewer … --save-scene` reproduce the
      checked-in files byte-for-byte;
    - the existing code-path goldens still pass.
17. **Restore renders identically:**
    - Render cornell deferred, snapshot, move a sphere, render, restore and render.
      The final image must be bit-identical to the pre-move one.
    - The restore frame must upload 0 instance bytes (`RenderScene::stats()`).
18. **Validation:** `ohao run` with a render under `OHAO_VALIDATION=1` reports zero
    messages for both scenes (deferred). `check_validation.py` stays clean.
19. **Benchmarks:** `scene_bench --check` passes every budget in "Performance".

## Done criteria

All verified by running, none estimated.

- Tests 1–19 pass. All existing suites and goldens stay green.
- `ohao schema` emits both files, and an agent-facing guide `docs/agents/ohao-cli.md`
  describes the CLI, the methods, the commands and the error codes. It was checked by
  running every example in it.
- `AGENTS.md` (+ `CLAUDE.md` import), `docs/ARCHITECTURE.md` and the refreshed
  `.claude/skills/` exist. Every command in them has been run, and the add-component
  recipe has been followed once end to end. No skill mentions Godot or scons.
- `OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200` still exits 0.
- The benchmark results (median/p95) are recorded in the change record.
- The monograph covers the new units with every citation resolving. The roadmap row
  is marked done. A `docs/changes/` record is written by a Haiku agent and reviewed.

## Build order

Each step ends with the engine running and every suite green.

1. `ohao_reflect`: registry, typed codegen, attributes, catalogue and JSON Schema
   export, the canonical writer (compact + pretty). Boost.PFR (test-only) fetched with a
   pinned tag. Tests 1, 12 (partly), 13. *(Done 2026-10-22. Each other dependency is
   fetched by the step that first uses it, so no step carries an unused one.)*
2. `EntityId` (splitmix64 generator, `seedIds`, `findById`; unordered_dense fetched
   here). Register all current authoring components with docs, units and limits.
   Test 5. *(Done 2026-10-23. Runtime components are registered too, as `.runtime()`,
   so the catalogue describes every component. The engine catalogue and scene schema
   have goldens in `tests/scene/golden`.)*
3. simdjson reader (fetched here), two-phase load, structured validation, error corpus.
   Tests 2–4, 6, 7. *(Done 2026-10-23. Also: the single-pass pretty writer, fast_float
   for float parsing, and the error-code table in the catalogue.)*
4. Versioning and migrations, `tests/scenes/v1` corpus. Test 8. *(Done 2026-10-23.)*
5. `ohao/api`: commands with inverses, query, the method table. Test 9. *(Done
   2026-10-23. Also: the token budget. Canonical files omit defaults; `world.query`
   filters and projects; `world.summary`; terse capped errors. Batch validation moved
   into the sequential apply with rollback. `Session` is the dispatch; the CLI (step 9)
   wraps it.)*
6. Columnar snapshot/restore, `scene_bench` and the budgets. Tests 10 and 19 for the
   operations that exist so far.
7. `MeshSource`, `MaterialOverride`, `generateQuad`/`generateBox`, instantiate, resolver.
   Test 11.
8. `Camera`, `Environment`, scene view. `cornell_box`/`model_viewer` converted, with
   `--save-scene`; their existing goldens must still pass.
9. `ohao` CLI, checked-in scenes, scene goldens, CLI goldens, restore-render test.
   Tests 14–18, then the full set for 19.
10. Agent guide, `AGENTS.md`/`CLAUDE.md`, `docs/ARCHITECTURE.md`, skills refresh,
    monograph units, roadmap, change record.

## Risks

- **Camera parity.** The renderer camera takes Euler pitch/yaw, and quaternion →
  pitch/yaw may be off by an ulp, which would break the exact Cornell goldens. Test 15
  catches it at step 8. The fallback is `Camera::setOrientation(front, up)` on the
  renderer camera, ledgered as a ruling.
- **Shared meshes.** Caching meshes by `MeshSource` turns the two Cornell spheres into
  one shared BLAS. Traversal should be unchanged, but BLAS float ordering might not be.
  The exact RT golden decides. The fallback is per-entity meshes as today, with sharing
  recorded as a follow-up.
- **The budgets are estimates until step 6 measures them.** They were sized from
  simdjson and `to_chars` throughput with headroom. If one is missed, profile first. A
  budget change needs Frank's OK.
- **Windows timing noise.** Medians of 50 after warm-up, high process priority, and p95
  reported. The gate compares medians only.
- **Three new header-only or small dependencies** (simdjson, unordered_dense, Boost.PFR
  test-only). All are pinned. simdjson's padding and forward-only On-Demand API shape
  the reader (raw tokens for floats, `raw_json()` for preserved components).
- **CLI start-up cost** (process plus Vulkan initialization for renders) limits
  one-process-per-operation agents. In 3, `--script` batches many operations per
  process. Later, `ohao serve` keeps one process alive.
- **Scope.** The command layer and CLI are the minimum that makes agents real users.
  The undo UI, RPC transport, C ABI, semantic diff and cooked format stay out of 3.
- **The opaque-preserve policy can carry stale data forever.** Accepted. Load reports
  count preserved components, and the editor can show them later.

## Review focus

Inputs no step's tests exercise by default, most likely to bite first:

1. An agent file with several mistakes at once: a misspelled field, a wrong enum, a
   missing parent, and a value out of range. Every error must be reported with exact
   paths and a suggestion, and the world must be untouched.
2. A command batch whose fifth command fails after four valid ones. Nothing may be
   applied, and the inverse of a successful batch must restore byte-identically.
3. A restore after structural changes (entities destroyed and new ones created). The
   recreated entities must keep their original `CreationOrder`, so draw order, light
   order and the next frame's pixels match.
4. A float written by one build and read by another. There must be no `double` detour
   anywhere in the reader or in the command path.
5. Two entities sharing a cached mesh, where one is destroyed or restored away. The
   other must keep rendering, and the mesh must not be released early.

## Decisions record

Decided with Frank on 2026-10-22, as recommended:

| # | Decision | Choice |
|---|---|---|
| 1 | Type registry | Own registry, member-pointer registration, editor attributes. Revised: per-member typed codegen. |
| 2 | Completeness guard | Boost.PFR (test-only). Revised: also checks docs and units. |
| 3 | Text format | Canonical JSON, own writer. Revised: simdjson parser; pretty and compact renderings. |
| 4 | Extension | `.scene.json` (revised 2026-10-23 from `.ohscene`; see "File naming") |
| 5 | Entity IDs | Random 64-bit. Revised: splitmix64(seed + counter), counter in snapshots, explicit IDs allowed. |
| 6 | Asset references | `AssetRef {path, guid}` + `AssetResolver`. Texture overrides deferred to the asset DB. |
| 7 | Inline geometry | Parametric primitives (`QuadPrim`, `BoxPrim` + generator shapes) |
| 8 | Camera | `Camera` component, applied by the host |
| 9 | Scene settings | `Environment` component. Render mode and denoiser stay host/CLI settings. |
| 10 | Unknown components | Preserved opaque. Revised: unknown *fields* are errors with suggestions. |
| 11 | File errors | Fail the whole load, world untouched. Revised: report all errors (≤ 100) with structured objects. |
| 12 | Snapshot/restore | In scope. Revised: columnar, diff-based, memcpy for POD. |
| 13 | Done criterion | Scenes ship as files; goldens render from them (now via `ohao run`) |
| 14 | Scripting | Out of scope for 3. The schema export serves agents and tools in any language. |
| 15 | Process | Spec → plan → subagent-driven tasks + final review, then a Haiku change record |

Added for agents, performance and languages. Frank asked Claude to own the language and
technology calls; these were decided on that delegation:

| # | Decision | Choice |
|---|---|---|
| 16 | Agents as users | First-class. Catalogue + JSON Schema, structured errors, commands, deterministic headless CLI. |
| 17 | One API | `ohao/api` method table shared by the CLI, future RPC, C ABI and editor. Authoring edits go only through commands. |
| 18 | Edits | Typed commands, atomic batches, inverse returned (undo = replay inverses) |
| 19 | CLI | `ohao` with `schema`/`validate`/`fmt`/`apply`/`query`/`summary`/`run`; JSON on stdout; JSON-Lines scripts = JSON-RPC requests minus the envelope |
| 20 | Control surface | Designed now (JSON-RPC 2.0 over stdio, MCP-native, `Content-Length` framing), built in sub-project 4 |
| 21 | Determinism | World state and CPU stdout bit-identical per build, inputs and seed. Pixels within golden tolerances. |
| 22 | Performance gate | `scene_bench` budgets (bench10k), medians gated; budget changes need Frank's OK |
| 23 | Reflection on hot paths | Never. Dependency rule tested. |
| 24 | Text vs speed | Three tiers: text for authoring, in-memory snapshots for agents' hot loops, cooked binary later (snapshot layout) |
| 25 | Core language | Keep C++20. No rewrite. |
| 26 | New engine layers, tools, control surface | C++20. No TypeScript, no GC or interpreted runtime on hot paths. |
| 27 | Language-neutral boundaries | Scene format + JSON Schema, catalogue, protocol, C ABI (later), cooked formats (later) |
| 28 | Agent client bindings | None required (MCP). Optional thin Python wrapper only, never on a hot path. |
| 29 | Editor UI | C++ + Dear ImGui (docking) on the Vulkan backend, as an `ohao/api` client |
| 30 | Shaders | GLSL stays; new shaders in Slang from 2b |
| 31 | Second compiled language | None now. Rust or Zig only for a standalone out-of-process tool behind a boundary, if it needs a library C++ lacks. |
| 32 | Dependencies | simdjson (scene reader), ankerl::unordered_dense (ID map), fast_float (float parsing, header-only, private to `ohao_reflect`), Boost.PFR (test-only); all pinned |
| 33 | Agent dev ergonomics in 3 | `AGENTS.md` (+ `CLAUDE.md` import), `docs/ARCHITECTURE.md` with recipes, refreshed `.claude/skills/`, JSON output from everything new; the rest in sub-project 4 |

Answered by Frank on 2026-10-22:

| # | Decision | Choice |
|---|---|---|
| 34 | Scripting | No external scripting project is involved. Scripting is out of scope for 3. **Proposed direction, not a commitment:** native C++ gameplay systems with DLL hot reload first, then a sandboxed WebAssembly tier for agent- and mod-written code. |
| 35 | After 3 | Sub-project 4 = agent control surface (`ohao serve`, JSON-RPC/MCP) + agent dev ergonomics. "Agentic" means both G1 (agents build games with OHAO) and G2 (agents develop OHAO). |
| 36 | Benchmark machine | Frank's dev laptop runs the performance gates |

## Open decisions

None.
