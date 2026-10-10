---
module: scene
id: transform-system
title: Transform propagation
standard: v2
---

## Two passes, in a fixed order

`updateTransforms` is the first system every `World::update` runs. It does two things, and the order between them is the whole design of the motion history:

1. **Settle.** Every entity tagged `MovedLastFrame` that is *not* dirty again this frame gets `prevWorld = world`, loses the tag, and is tagged `RenderDirty`.
2. **Propagate.** Every `TransformDirty` entity with no dirty ancestor is a root; each root's subtree is recomputed.

{{cite ohao/ecs/transform_system.cpp "// 1. Last frame's movers that did not move again: prevWorld catches up."}}
{{cite ohao/ecs/transform_system.cpp "if (!hasDirtyAncestor(reg, e)) roots.push_back(e);"}}

Settling first matters for an entity that moves on consecutive frames: it is still `TransformDirty`, so the settle pass skips it and the propagate pass rotates `world` into `prevWorld` itself. Settle only ever touches entities that stopped.

## Parent-first without a sort

The world matrix of entity $e$ with parent $p$ is

$$W_e = W_p\,M_{\text{local}}(e), \qquad W_{\text{root}} = M_{\text{local}}(\text{root})$$

and the only ordering requirement is that $W_p$ is current before $W_e$ is computed. The system gets that without sorting anything by depth. Each root is walked depth-first from an explicit stack: an entity's world is computed *before* its children are pushed, so by the time a child is popped its parent's `WorldTransform` already holds this frame's value.

{{cite ohao/ecs/transform_system.cpp "const glm::mat4 world = parentWorld(reg, e) * reg.get<Transform>(e).matrix();"}}

Children are pushed unconditionally — a moved parent moves its whole subtree, whether or not any child was edited. Root selection is what keeps the work linear in the dirty subtrees: an entity with a dirty ancestor is not a root, because the ancestor's walk will reach it, so no subtree is computed twice in one update. The price is the ancestor test itself, one `Parent` lookup per level for every dirty entity:

$$\text{cost} = \sum_{e\,\in\,\text{dirty}} \operatorname{depth}(e) \;+\; \sum_{r\,\in\,\text{roots}} |\operatorname{subtree}(r)|$$

For the flat scenes every example builds — no entity has a parent — the first sum is zero and the second is the number of entities that changed.

An entity whose parent is gone, or has no `WorldTransform` yet, composes against identity rather than failing:

{{cite ohao/ecs/transform_system.cpp "if (const auto* wt = reg.try_get<WorldTransform>(p->entity)) return wt->world;"}}

## Motion history: prevWorld and the settle frame

Each recompute of an entity that already has a `WorldTransform` copies the old matrix into `prevWorld` before writing the new one, and tags `MovedLastFrame`:

{{cite ohao/ecs/transform_system.cpp "wt->prevWorld = wt->world;"}}

The G-buffer vertex shader projects the vertex through both, so an object's own displacement between frames lands in the velocity buffer:

{{cite shaders/core/gbuffer.vert "fragPrevPos = cam.prevViewProj * (inst.prevWorld * vec4(inPosition, 1.0));"}}

The settle pass exists because the GPU copy of an instance is only rewritten when something marks it `RenderDirty`. Without it, an object that moves once and stops would keep its last `prevWorld ≠ world` pair on the GPU indefinitely, and the velocity buffer would report the final step's motion on every later frame. With it, a single move costs exactly two instance uploads: the move frame, carrying the displacement, and the next frame, carrying `prevWorld = world` and zero velocity.

`MovedLastFrame` is set on *every* recompute, not only when the matrix changed: re-patching a `Transform` with its current value, or reparenting under an identity parent, still costs the two-frame tail. Nothing reaches the GPU for it, though — the instance table compares the new `InstanceGpu` against the old bytes and marks nothing dirty when they match.

{{cite ohao/render/scene/instance_table.cpp "if (std::memcmp(&cur, &g, sizeof(g)) != 0) {"}}

Every recomputed entity is tagged `RenderDirty`, renderable or not. That is how a moved `Light` reaches the light rebuild: the render extract treats any `RenderDirty` entity carrying a light as a reason to rebuild the light list.

## The patch rule and its check

`TransformDirty` is set by EnTT's update signal on `Transform`, which fires for `patch`, `replace` and `emplace_or_replace` — and not for a write through the reference `get<Transform>()` returns. Such a write changes the local transform and leaves the world matrix, the GPU instance and the TLAS on the old pose, with nothing logged.

{{cite ohao/ecs/components.hpp "// a plain get<>() write is not tracked (see verifyTransforms)."}}

`verifyTransforms` is the detector. For every entity that has both transforms and is not dirty, it rebuilds $W_p M_{\text{local}}$ and compares it element by element with the stored `world`; any element off by more than $10^{-4}$ counts the entity once.

{{cite ohao/ecs/transform_system.cpp "if (std::fabs(expect[c][r] - wt.world[c][r]) > 1e-4f) { ++bad; goto next; }"}}

It compares against the parent's *stored* world, not a recomputed one, so an untracked write to a parent is counted once — at the parent — and its children, still consistent with the parent's stale matrix, are not. The count therefore names the entities that were written wrongly, not everything that is visibly wrong.

:::key
`verifyTransforms` is a function, not a build mode: no system or frame loop calls it, and the only caller in the tree is the ECS test suite. The patch rule is enforced by convention and by tests that seed exactly one bad write; a game that wants the check at run time has to call it itself.
:::

{{cite tests/ecs/ecs_tests.cpp "CHECK(verifyTransforms(w.registry()) == 1, "an untracked write is reported");"}}

## Contracts

- `updateTransforms` clears `TransformDirty` and sets `RenderDirty`; it never clears `RenderDirty`. The render extract clears it, so a world without the extract installed accumulates the tag on every entity that ever moved.
- The first update after `create` emplaces `WorldTransform{world, world}` and does not tag `MovedLastFrame`: spawning is not motion.
- Propagation is single-threaded and runs on the caller's thread inside `World::update`. Nothing else may touch the registry concurrently.
