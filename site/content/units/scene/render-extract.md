---
module: scene
id: render-extract
title: Render extract
standard: v2
---

## The one bridge from registry to GPU

The renderer never includes EnTT. Everything it draws or traces lives in a `render::RenderScene`, and the only code that writes entity state into it is the `render_extract` system, installed by `ecs::installRenderExtract`. It runs once per `World::update`, immediately after the transform system, and is incremental: it does nothing for an entity that did not change.

{{cite ohao/ecs/render_extract.hpp "void installRenderExtract(World& world, render::RenderScene& scene);"}}

Installation is guarded by a flag in the registry context, so a second call on the same world logs and returns rather than registering a second set of signal handlers and a second owner for the same slots. The state lives in an `ExtractState` held by `shared_ptr` inside the system's closure — its lifetime is the system's, and the system's lifetime is the world's.

## What marks work

The extract keeps no list of "renderables to check". Work arrives as the `RenderDirty` tag and as EnTT signals it connects at construction:

- `MeshRenderer` constructed or updated → `RenderDirty` on the entity.
- `MeshRenderer` or `RenderSlot` destroyed → the entity's slot is queued for release.
- `Light` or `EmissiveProxyLight` constructed, updated or destroyed → the light list is marked dirty.
- The transform system tags `RenderDirty` on every entity whose world matrix it recomputed, including the settle frame after a move.
- Installation itself, when the world already has renderables or lights: a host that starts its renderer after loading a scene (`ohao run --render`) installs the extract into a populated world, and the constructor tags every existing `MeshRenderer` and marks the lights, so the first run picks them up.

{{cite ohao/ecs/render_extract.cpp "reg.on_update<MeshRenderer>().connect<&ExtractState::markRenderDirty>(this);"}}
{{cite ohao/ecs/render_extract.cpp "for (const entt::entity e : reg.view<MeshRenderer>()) reg.emplace_or_replace<RenderDirty>(e);"}}

The same write rule as `Transform` follows: a `MeshRenderer` change must go through `patch` or `replace`. Hiding an entity with `get<MeshRenderer>().visible = false` is not seen — the instance stays visible, in raster and in the TLAS, until something else dirties the entity.

## One update, five steps

**1. Releases.** Destroy signals do not call into the `RenderScene`; they only queue. The queued slots are released first, and for an entity that is still alive the step repairs its components — a stale `RenderSlot` left by a removed `MeshRenderer` is removed, and an entity that lost only its `RenderSlot` but still has a `MeshRenderer` is re-dirtied so it gets a new one.

**2–3. Updates and creations.** Every `RenderDirty` entity with a `MeshRenderer`, a `WorldTransform` and a usable mesh is either updated in place, if it already owns a slot, or collected for creation. Collected entities are sorted by their `CreationOrder` before any slot is requested:

{{cite ohao/ecs/render_extract.cpp "// Slots in creation order, independent of pool iteration order."}}

Slot numbers are not cosmetic. A slot is the instance's index in the instance buffer, the slot its raster draws' records name (and so the order of the draw table) and its `instanceCustomIndex` in the TLAS, so the order in which entities receive slots fixes the order of everything downstream. EnTT's `view<RenderDirty>()` iterates the tag's pool, whose order depends on the history of insertions and swap-and-pop removals; sorting by the never-recycled creation sequence makes a scene built by the same code get the same slots on every run.

Because releases run before creations, a slot freed this update is the first one handed to this update's first new entity — the instance table reuses the lowest free slot. The `RenderScene` treats that as a structural change and rebuilds the TLAS rather than refitting it, so the reused slot never inherits the old instance's BLAS reference.

**4. Clear.** `RenderDirty` is cleared on every entity, including those the extract skipped.

**5. Lights.** If a light component changed, or any `RenderDirty` entity carries one, the whole light list is rebuilt.

## Slot ownership

The extract records which entity it gave each slot to, and it only ever updates a slot whose owner matches:

{{cite ohao/ecs/render_extract.cpp "if (rs->slot < m_owner.size() && m_owner[rs->slot] == e)"}}

`RenderSlot` is an ordinary component, visible to game code, but it is the extract's alone to write. The owner check means a hand-written or copied `RenderSlot` can never redirect another entity's instance; the cost is that an entity carrying a forged one is neither updated (owner mismatch) nor given a real slot (it already has a `RenderSlot`), and simply never renders.

Teardown is the same ownership read in reverse. `World`'s member order destroys its systems before its registry, so `ExtractState`'s destructor still has a live registry to disconnect from, and it destroys every instance it owns and clears the light list it set. That is why the host must drop the world before the renderer:

{{cite ohao/runtime/live_host.cpp "m_world.reset();   // releases its RenderScene slots while the RenderScene exists"}}

## Lights, in creation order

A rebuild walks every `Light` and every `EmissiveProxyLight` with a `WorldTransform` and resolves them to world space. Position is the world matrix's translation column; direction is the entity's $-Z$ axis carried through the upper 3×3 basis $B$ and renormalised; an area rectangle's edges go through $B$ unnormalised:

$$\mathbf{d} = \frac{B\,(0,0,-1)^{\mathsf T}}{\lVert B\,(0,0,-1)^{\mathsf T}\rVert}, \qquad \mathbf{e}_i^{\text{world}} = B\,\mathbf{e}_i$$

so scaling an area light's entity scales its emitting area, and with it the area term the RT light carries. An `EmissiveProxyLight`'s centre goes through the full world matrix, but its radius does not: a model spawned at scale 3 keeps the proxy radius computed from its unscaled vertices.

{{cite ohao/ecs/render_extract.cpp "d.direction = glm::normalize(basis * glm::vec3(0.0f, 0.0f, -1.0f));"}}
{{cite ohao/ecs/render_extract.cpp "d.position = glm::vec3(wt.world * glm::vec4(p.localCenter, 1.0f));"}}

Each `LightDesc` carries its entity's creation sequence as `order`, and the light table stable-sorts on it. The order is load-bearing, not tidiness: the raster light buffer holds eight lights, and the first point or spot light in it is the one that gets the shadow map. With pool order instead, destroying an unrelated light could change which lights are in the eight and which one casts shadows.

{{cite ohao/render/scene/light_table.cpp "std::stable_sort(sorted.begin(), sorted.end(),"}}

A moving light rebuilds the whole list twice — on the move frame and on its settle frame — because it is `RenderDirty` both times. The second rebuild produces identical RT bytes, and the light table compares them and uploads nothing.

## Stale and unknown mesh handles

An entity whose `MeshHandle` the `RenderScene` does not recognise is skipped: no slot, no draw, no TLAS instance. Each distinct handle is reported once, keyed on its index and generation, so a thousand entities sharing one bad handle produce one line:

{{cite ohao/ecs/render_extract.cpp "if (!meshUsable(mr->mesh)) continue;   // stale/unknown: skipped, logged once"}}

In this tree "stale" mostly means "never valid". The mesh registry never unregisters a mesh and issues every handle at generation 0, so the handles that fail are the default `MeshRenderer{}` handle, the empty handle `registerMesh` returns when it rejects a mesh, and handles taken from a different `RenderScene`. Material handles are not checked here; the instance table maps any unknown material index to the default material instead.

:::key
The skip is final until something re-dirties the entity. Step 4 clears `RenderDirty` on skipped entities too, so assigning a valid mesh later has to go through `patch<MeshRenderer>()` — which it must anyway — for the entity to be picked up.
:::

## Contracts

- The `RenderScene` must outlive the `World`. Destroying the world releases its slots and clears its lights through the scene. `VulkanRenderer::shutdown()` only shuts the scene down; the object lives, inert, until the renderer is destroyed, so a world released after an explicit shutdown is safe.
- `MeshRenderer`, `Light`, `EmissiveProxyLight` and `GiMaterialLegacy` are seen only through EnTT signals: write them with `patch` or `replace` (or emplace/remove them). A write through `get<>()` never reaches the scene.
- At most one extract per world. One `RenderScene` may serve several worlds, but each world's light rebuild replaces the whole light list, so only one of them can own lights.
- A slot is released at the start of the next `render_extract` run, not when the entity is destroyed. A frame rendered between the two still draws and traces the instance at its last pose; the `LiveHost` loop destroys in its hooks (or a request it pumped) and runs `World::sync()` before every `render()`, so it never renders in that gap.
