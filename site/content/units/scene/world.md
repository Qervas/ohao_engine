---
module: scene
id: world
title: ECS world & hierarchy
standard: v2
---

## An entity is six components before it is anything

Game state lives in an EnTT registry owned by `ohao::ecs::World`. `World::create` (or `createWithId`, for an id from a file or a command) is the only sanctioned way to make an entity, and it never returns a bare handle: every entity leaves it carrying a stable `EntityId`, a `Name`, a local `Transform`, an empty `Children` list, the `TransformDirty` tag and a `CreationOrder` stamp.

{{cite ohao/ecs/world.cpp "m_registry.emplace<EntityId>(e, id);"}}

The `EntityId` is what files, commands and snapshots refer to; the reflection module's scene-files unit covers how it is generated and kept unique.

{{cite ohao/ecs/world.cpp "m_registry.emplace<CreationOrder>(e, m_nextSeq++);"}}

What it does *not* carry yet is a `WorldTransform`. That component is created by the transform system on the next `World::update`, with `prevWorld` equal to `world`, so an entity's first rendered frame never reports motion. Everything that reads world space — the render extract, the light rebuild — views over `WorldTransform` and therefore cannot see an entity in the window between `create` and the first update.

`CreationOrder` exists because EnTT ids are not an order. The registry recycles an id after `destroy`, with a bumped version, and its component pools reorder on swap-and-pop removal, so neither the id value nor pool iteration says which of two entities came first. The sequence is a plain 64-bit counter that is never reused:

{{cite ohao/ecs/components.hpp "Unlike entity ids it is never"}}

It is the tiebreaker the render extract uses for instance slots and the light table uses for light order — the two places where iteration order would otherwise leak into the image.

## The components

`ohao/ecs/components.hpp` is the whole vocabulary. It splits into what a scene file saves (authoring components) and what the engine derives each frame (runtime components); the type registry records which is which.

Authoring components:

- `Name`, `Transform` (local position, unit quaternion, per-axis scale), `Parent`.
- `Light` — type, color, intensity, range, radius, cone angles, area-rect edges. No position or direction field: both come from `WorldTransform`.
- `MeshSource` and `MaterialOverride` — where the mesh comes from (a model asset or a primitive) and the material of a primitive; the instantiate system turns them into a `MeshRenderer`.
- `Camera` and `Environment` — the view and the image-based lighting, one of each per world.
- `Spin` — a constant turn about a local axis, applied by the spin system.
- `InputActions` — the world's controls (action and axis bindings by key name), one per world; the World rebuilds its `InputMap` from it when it changes.
- `FlyController` — flies its entity from named axes and actions, applied by the `fly_controller` system.

Runtime components:

- `WorldTransform` — this frame's object-to-world matrix and last frame's.
- `Children` — the hierarchy's other direction.
- `MeshRenderer` — a `MeshHandle`, one `MaterialHandle` per submesh, a `visible` flag.
- `EmissiveProxyLight` — a point light standing in for a mesh's emissive texture, in model space.
- `GiMaterialLegacy` — the RT GI bounce-albedo word copied into each instance.
- `RenderSlot`, `CreationOrder`, `InstantiateError`, `PreservedComponents` (unknown components a file carried, written back on save).

and the empty tags `TransformDirty`, `RenderDirty`, `MovedLastFrame` and `InstantiateDirty`.

A `MeshRenderer` may list fewer materials than its mesh has submeshes. That is legal, not an error path: the missing submeshes render with material 0, the material table's default.

{{cite ohao/ecs/components.hpp "// One material per submesh, in submesh order. Fewer entries than submeshes is legal:"}}

The local matrix is the usual ordered product, built fresh from the three fields each time it is asked for:

$$M_{\text{local}} = T(\mathbf{p})\;R(q)\;S(\mathbf{s})$$

With GLM's column-vector convention the rightmost factor applies first, so scale acts along the entity's own axes, rotation orients it, translation places it. There is no cached local matrix and no shear: a TRS triple is all a `Transform` can hold. A non-uniform scale under a rotated child still composes into a sheared *world* matrix — `WorldTransform` stores the full `mat4`, so nothing is lost — but no `Transform` can be written back from it.

{{cite ohao/ecs/world.cpp "return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) *"}}

## Two phases, each in the order its systems were added

Systems come in two phases, two vectors walked in order — no discovery, no dependency graph, no sort. **Simulate** systems change the world and run once per fixed step with the fixed dt (`World::simulate(dt)`); **sync** systems derive state from it and run once per batch of steps and before each render, with no dt (`World::sync()`). The constructor adds the engine-provided `spin` and `fly_controller` as simulate systems and `transform` as a sync system; a host that owns a renderer inserts its two sync systems around `transform` by name:

{{cite ohao/ecs/world.cpp "addSimulate("spin", [](World& w, float dt) { updateSpin(w.registry(), dt); });"}}
{{cite ohao/ecs/world.cpp "addSync("transform", [](World& w) { updateTransforms(w.registry()); });"}}
{{cite ohao/ecs/instantiate.cpp "world.addSyncBefore("transform", "instantiate", [state](World&) { state->run(); });"}}
{{cite ohao/ecs/render_extract.cpp "world.addSyncAfter(anchor, "render_extract", [state](World&) { state->run(); });"}}

The extract's anchor is the last presentation system already installed: `view_model` (the
first-person gun, which combat installs), else `interpolate` (physics), else `transform`:

{{cite ohao/ecs/render_extract.cpp "const char* anchor = has("view_model") ? "view_model" : has("interpolate") ? "interpolate" : "transform";"}}

With everything installed, a step runs `spin` then `fly_controller` (after the World's `InputMap` has taken that step's events, `World::stepInput`), and a sync runs `instantiate`, `transform`, `render_extract`. Everything the simulate phase moved reaches the world matrices and the GPU at the next sync. Instantiate runs before `transform`, so an entity whose mesh it just built gets its world matrix — and with it a render slot — in that same sync. `World::update(dt)` is one `simulate(dt)` then one `sync()`, for tests and tools.

The split is what makes a frame's simulation independent of how often it is rendered: `world.step` with 100 frames simulates 100 times and syncs once, a window host simulates once per fixed step of wall time and syncs once per presented frame, and a capture only syncs — rendering never advances the simulation. A realtime session and a lockstep one that run the same steps reach the same state, bit for bit.

In the window host each step is `Session::step`, which hands the step's input to the world and then simulates once:

{{cite ohao/runtime/live_host.cpp "m_session->step(m_events, fixedDt);"}}
{{cite ohao/api/input.cpp "s.world.simulate(dt);"}}

`addSyncAfter` and `addSyncBefore` with a name that is not in the list append instead of failing; plain `addSync` always appends — once the extract is installed, *after* `render_extract`, so what such a system writes reaches the GPU one sync late. A sync system cannot take a dt by its type, so stepping behaviour cannot hide there.

:::why
Fixed lists instead of a scheduler: with five systems and no threading, an explicit order is something a reader can verify by looking at two vectors (`simulateNames()`, `syncNames()`). A dependency-sorted scheduler earns its cost when there are dozens of systems and parallel stages; neither exists yet.
:::

## Hierarchy: both directions, cycles rejected

The hierarchy is stored twice — `Parent` on the child, the child's id in the parent's `Children` vector — and `World::setParent` is the only code that keeps the two in step. It walks up from the proposed parent and refuses the edit if it meets the child, so no entity can become its own ancestor:

{{cite ohao/ecs/world.cpp "if (a == child) return false;"}}

The walk costs one `try_get<Parent>` per level of the new parent's depth. On success the child is erased from its old parent's `Children` (a linear `std::erase`), appended to the new one, and tagged `TransformDirty`. The local `Transform` is kept as it was, so reparenting moves the entity in world space; there is no "keep world position" variant.

Two edges of that contract are easy to miss:

- A non-null parent that is no longer valid is not rejected. Both the cycle walk and the attach are skipped, the old link is removed, and the call returns `true` — a dead parent handle detaches the child.
- `Children` order is attach order, and nothing sorts it. Transform propagation does not depend on it; anything that walks children for presentation does.

## Destroying a subtree

`World::destroy` first detaches the entity from its parent, then collects its whole subtree depth-first and destroys the collected list back to front, so every entity is destroyed after all of its descendants:

{{cite ohao/ecs/world.cpp "for (auto it = order.rbegin(); it != order.rend(); ++it) m_registry.destroy(*it);"}}

No survivor is left holding a `Parent` that names a dead entity, and the surviving parent's `Children` no longer lists it. Each destroyed entity's `MeshRenderer` fires EnTT's destroy signal on the way out, which is how its GPU instance slot gets released (see the render-extract unit).

:::key
`World` owns three invariants — every entity has `Transform` and `CreationOrder`, `Parent` and `Children` agree, and no cycle exists. Its methods enforce all three. The raw registry keeps the second one on the destroy path: `World` connects destroy hooks to `Parent` and `Children`, so `registry().destroy(e)` removes `e` from its parent's `Children`, and its children are detached (they become roots, keeping their local transforms) rather than destroyed. `World::destroy` is the preferred path, because it removes the whole subtree. Nothing guards `emplace<Parent>` by hand; parenting goes through `setParent`.
:::

## Contracts

- Destroy with `World::destroy` (the subtree goes with the entity). `registry().destroy(e)` and `registry().clear()` keep `Parent`/`Children` consistent through the hooks but detach children instead of destroying them.

{{cite ohao/ecs/world.cpp "void World::onChildrenDestroyed(entt::registry& r, entt::entity parent) {"}}

- Write `Transform` with `World::setTransform` or `registry().patch<Transform>()`. Both fire EnTT's update signal, which the world turns into `TransformDirty`; a write through `get<Transform>()` is not seen (the transform-system unit covers the check that catches it).

{{cite ohao/ecs/world.cpp "m_registry.on_update<Transform>().connect<&World::onTransformChanged>(this);"}}

- `World` is non-copyable and owns its systems; destroying it destroys `m_simulate` and `m_sync` before `m_registry`, which is what lets the render extract release its slots while the registry still exists.
- `CreationOrder` is set once and never rewritten. Do not replace it to "reorder" lights or instances — slots already handed out keep their numbers.
