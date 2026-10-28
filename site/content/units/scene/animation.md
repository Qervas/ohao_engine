---
module: scene
id: animation
title: Animation: saved integers, a pose per frame, a mesh per instance
standard: v2
---

## State is integers, a pose is presentation

An `Animator` holds no time and no pose. Each layer saves its state's name, the steps spent in
it, the state it fades from with that one's steps, and the fade's length and progress, all
integers. The `animation` simulate system advances them once per fixed step, in id order:

{{cite ohao/ecs/animation.cpp "Animator& a = reg.get<Animator>(e);   // the step's own write: no signal, not a placement"}}

Clip time is never accumulated, so it never drifts, and a snapshot, a file and `world.hash`
hold an animation exactly. The pose is derived at render rate by the `pose` sync system, from
those integers and the frame's interpolation alpha:

{{cite ohao/anim/anim_graph.cpp "const float steps = std::max(0.0f, static_cast<float>(st.steps) - 1.0f + alpha);"}}

With $n$ the steps in the state, $\alpha$ the render alpha, $\Delta t = 1/60$ s, $r$ the state's
and the clip's rate and $t_0$ the state's start offset, a frame samples its clip at

$$t = t_0 + (n - 1 + \alpha)\, \Delta t\, r$$

so a lockstep frame ($\alpha = 1$) shows the newest step exactly and a realtime frame between
steps shows the motion between them. The pose (`AnimPose`) is runtime: never saved, never
hashed.

:::why
Saving a float clip time would put the sum of every step's increment in the hash, and two hosts
that reach the same step by different routes (a restore, a replay, a restart) would carry
different roundings. Counting steps makes the step count the time; the pose is a function of it.
:::

## Drivers write parameters from saved state

A graph's parameters are written by named drivers in code, at the start of the step, from saved
components only: the enemy's from its AI state and the step's combat events, the player's arms'
from the owner's weapon and body. A restart restores the round's start inside the combat step,
before the animation step, so a driver reads only the events after the step's last restart:

{{cite ohao/ecs/enemy_anim.cpp "if (stepEvents[i].kind == CombatEvent::Kind::Restart) first = i + 1;"}}

The restart step then steps the restored Animators exactly as the round's first step did, and
`world.hash` after it is the round start's.

A clip can take its rate from a parameter. The arms' reload clip names `rateFrom: {param:
reloadTime, at: mag_in}`: it plays so that the magazine goes home when the weapon finishes
reloading, at any `reloadTime`. The anchor $a$ (the event's time) lands $L$ seconds in:

{{cite ohao/anim/anim_graph.cpp "return state.startAt + c->rateAnchor * state.rate * (steps * kStepSeconds / len);"}}

$$t = t_0 + a \cdot r \cdot \frac{n\, \Delta t}{L}$$

Written as a fraction of $L$, $t$ reaches $a$ exactly when $n\,\Delta t$ reaches $L$, not one
rounding later.

## A skinned instance is its own mesh

When the render extract first meets an entity with `SkinnedMesh`, it gives the instance a mesh
of its own: a vertex range the size of the base mesh's, the base's indices and material slots,
copies of its meshlets, and an updatable BLAS:

{{cite ohao/ecs/render_extract.cpp "const render::MeshHandle own = m_rs.registerSkinnedInstance(mr.mesh);"}}

Nothing downstream knows it is skinned. The G-buffer, culling, meshlets, shadows, the TLAS and
the path tracer read the instance's mesh like any other. Three passes run before the scene's
upload, and only for instances whose palette changed: `skin` deforms the vertices with the
palette, `skin.bounds` recomputes each meshlet's sphere, and `rt.blas.refit` refits the BLAS,
rebuilding it every 120 refits:

{{cite ohao/render/scene/render_scene.cpp "const bool rebuild = s.refits >= kBlasRefitsPerRebuild;"}}

Motion vectors need last frame's deformed position. The copy's vertices no longer need joints
or weights (the skin pass reads those from the base mesh), so the skin pass writes last frame's
position into the copy's `boneWeights.xyz` and the draw is flagged `kDrawSkinned`:

{{cite shaders/core/gbuffer.vert "const vec3 prevLocal = (d.info.y & kDrawSkinned) != 0u ? inBoneWeights.xyz : inPosition;"}}

## Attachments follow joints

A `JointAttach` child (an enemy's rifle in the right hand, the muzzle light on the arms' gun
joint) is drawn at its parent's drawn pose, times the joint's model matrix, times its own
`Transform`, and its whole subtree with it:

{{cite ohao/ecs/animation.cpp "draw(draw, c, parentWorld * pose->model[static_cast<std::size_t>(joint)] * reg.get<Transform>(c).matrix(), 0);"}}

The first-person arms are a `ViewModel`'s skinned child: `view_model` places the root in the
view, then `pose` poses the arms inside it and draws the attachments, so the rifle's muzzle
light sits at the barrel's tip in every frame.

:::key
An Animator saves integers: states, steps and fades, advanced once per step by drivers that read
saved state. The pose is sampled from them at render rate, never saved or hashed. A skinned
instance gets its own mesh, deformed on the GPU only when its palette changes, with last frame's
position kept in place of its weights for motion vectors. Every consumer reads it as any mesh.
:::

## Contracts

- A pose is presentation: `AnimPose` is runtime, and a lockstep frame shows the newest step's pose.
- Drivers write parameters from saved state and the step's combat events after its last restart.
- Nothing may read a skinned copy's joints or weights; its `boneWeights.xyz` is last frame's position.
- The previous palette is the previous sync's: captures and `world.step` start without motion.
- Clip events fire in the step, in id order, into a log of 1,024 (`animation.info`).
