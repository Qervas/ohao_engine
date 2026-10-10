---
module: scene
id: world
title: ECS world & hierarchy
standard: v2
---

## An entity is five components before it is anything

Game state lives in an EnTT registry owned by `ohao::ecs::World`. `World::create` is the only sanctioned way to make an entity, and it never returns a bare id: every entity leaves it carrying a `Name`, a local `Transform`, an empty `Children` list, the `TransformDirty` tag and a `CreationOrder` stamp.

{{cite ohao/ecs/world.cpp "m_registry.emplace<CreationOrder>(e, m_nextSeq++);"}}

What it does *not* carry yet is a `WorldTransform`. That component is created by the transform system on the next `World::update`, with `prevWorld` equal to `world`, so an entity's first rendered frame never reports motion. Everything that reads world space — the render extract, the light rebuild — views over `WorldTransform` and therefore cannot see an entity in the window between `create` and the first update.

`CreationOrder` exists because EnTT ids are not an order. The registry recycles an id after `destroy`, with a bumped version, and its component pools reorder on swap-and-pop removal, so neither the id value nor pool iteration says which of two entities came first. The sequence is a plain 64-bit counter that is never reused:

{{cite ohao/ecs/components.hpp "Unlike entity ids it is never"}}

It is the tiebreaker the render extract uses for instance slots and the light table uses for light order — the two places where iteration order would otherwise leak into the image.

## The components

`ohao/ecs/components.hpp` is the whole vocabulary. Data components:

- `Transform` — local position, unit quaternion, per-axis scale.
- `WorldTransform` — this frame's object-to-world matrix and last frame's.
- `Parent` / `Children` — the hierarchy, stored in both directions.
- `MeshRenderer` — a `MeshHandle`, one `MaterialHandle` per submesh, a `visible` flag.
- `Light` — type, color, intensity, range, radius, cone angles, area-rect edges. No position or direction field: both come from `WorldTransform`.
- `EmissiveProxyLight` — a point light standing in for a mesh's emissive texture, in model space.
- `GiMaterialLegacy` — the RT GI bounce-albedo word copied into each instance.

and three empty tags: `TransformDirty`, `RenderDirty`, `MovedLastFrame`.

A `MeshRenderer` may list fewer materials than its mesh has submeshes. That is legal, not an error path: the missing submeshes render with material 0, the material table's default.

{{cite ohao/ecs/components.hpp "// One material per submesh, in submesh order. Fewer entries than submeshes is legal:"}}

The local matrix is the usual ordered product, built fresh from the three fields each time it is asked for:

$$M_{\text{local}} = T(\mathbf{p})\;R(q)\;S(\mathbf{s})$$

With GLM's column-vector convention the rightmost factor applies first, so scale acts along the entity's own axes, rotation orients it, translation places it. There is no cached local matrix and no shear: a TRS triple is all a `Transform` can hold. A non-uniform scale under a rotated child still composes into a sheared *world* matrix — `WorldTransform` stores the full `mat4`, so nothing is lost — but no `Transform` can be written back from it.

{{cite ohao/ecs/world.cpp "return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) *"}}

## Systems run in the order they were added

A system is a `std::function<void(World&, float)>`. There is no discovery, no dependency graph, no sort: `World::update` walks a vector. The constructor puts `transform` in it first, and `installRenderExtract` inserts `render_extract` immediately after it with `addSystemAfter`:

{{cite ohao/ecs/world.cpp "addSystem("transform", [](World& w, float) { updateTransforms(w.registry()); });"}}
{{cite ohao/ecs/render_extract.cpp "world.addSystemAfter("transform", "render_extract","}}

`addSystemAfter` with a name that is not in the list appends instead of failing. Plain `addSystem` always appends — which, once the extract is installed, means *after* `render_extract`. A gameplay system registered that way and moving an entity is seen by the transform system and the extract only on the next `update`: one frame of latency, silently. Nothing in the tree registers such a system today. `ohao_game` moves its cube in the `Application`'s `update` hook, which runs before `World::update`:

{{cite ohao/runtime/application.cpp "m_world->update(frameDt);"}}

:::why
A fixed list instead of a scheduler: with two engine systems and no threading, an explicit order is something a reader can verify by looking at one vector, and the only ordering bug it admits — appending after the extract — is visible in `systemNames()`. A dependency-sorted scheduler earns its cost when there are dozens of systems and parallel stages; neither exists yet.
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
`World` owns three invariants — every entity has `Transform` and `CreationOrder`, `Parent` and `Children` agree, and no cycle exists — and enforces all three only through its own methods. `registry()` hands out the raw registry, so nothing stops game code from calling `registry().destroy(e)` on a parented entity or `emplace<Parent>` by hand. Do either and the transform system will push a dead id from a stale `Children` list onto its stack and `get<Transform>` it. The world API is the safe surface; the raw registry is for components the world does not own.
:::

## Contracts

- Write `Transform` with `World::setTransform` or `registry().patch<Transform>()`. Both fire EnTT's update signal, which the world turns into `TransformDirty`; a write through `get<Transform>()` is not seen (the transform-system unit covers the check that catches it).

{{cite ohao/ecs/world.cpp "m_registry.on_update<Transform>().connect<&World::onTransformChanged>(this);"}}

- `World` is non-copyable and owns its systems; destroying it destroys `m_systems` before `m_registry`, which is what lets the render extract release its slots while the registry still exists.
- `CreationOrder` is set once and never rewritten. Do not replace it to "reorder" lights or instances — slots already handed out keep their numbers.
