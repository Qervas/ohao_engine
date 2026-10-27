---
module: runtime
id: feel
title: Feel: the look lead, frame timing and the first-person gun
standard: v2
---

## The frame shows the look before a step takes it

A realtime window steps at a fixed 60 Hz and renders as fast as it can, so most frames run no
step. Before sub-project 9 a mouse movement waited for the next step to turn the player, and the
frames in between showed the old view. The look lead shows it at once. Each frame the host sums
the look no step has consumed yet, the window's `MouseDelta` events since the last step and the
`input.inject` movements due at the next one, in the order that step's input map will sum them:

{{cite ohao/runtime/live_host.cpp "if (e.type == InputEvent::Type::MouseDelta) pending += glm::vec2"}}

and hands it to the World, which keeps it beside the render alpha and clears it when a step runs:

{{cite ohao/ecs/world.hpp "void setPendingLook(glm::vec2 units) noexcept { m_pendingLook = units; }"}}

{{cite ohao/ecs/world.cpp "m_pendingLook = glm::vec2(0.0f);   // the step took it"}}

The `interpolate` sync system draws the input-driven, living player turned by that look. It does
not approximate the turn: it calls `applyLook`, the function the step itself calls, on the newest
step's yaw and pitch:

{{cite ohao/ecs/physics_scene.cpp "const LookResult look = applyLook(p.yawDeg, p.pitchDeg, pending, *pc);"}}

{{cite ohao/ecs/physics_scene.cpp "yawDeg -= delta.x * pc.lookDegPerUnit;"}}

So the next step lands exactly where the frame already was (`sp9_t19_lead_matches_next_step`, and
`sp9_t19_lead_split_across_steps` for a look the clock splits over two steps). The yaw goes on the
body and the pitch on the head; the rest of the subtree is carried along. Only the player's
position is interpolated between its last two step poses:

{{cite ohao/ecs/physics_scene.cpp "pos = glm::mix(mv->second.fromPos, mv->second.toPos, alpha);"}}

Its rotation is drawn from the newest step plus the lead, never slerped from the step before:
interpolating the turn would put back the latency the lead removes. All of it is `RenderPose`.
`Transform`, `world.hash`, saved files and snapshots stay the step's (`sp9_t19_lead_never_in_world`),
and there is no lead in lockstep (alpha 1, nothing pending), for a dead player, or for characters
moved by a `MoveIntent`.

## Timing a frame

A latency claim needs a clock on both ends. `FrameLog` stamps every realtime frame on one steady
clock: when its input was polled, when the view it draws was resolved, when it was submitted and
when it was presented, with the look movements it is the first to show in full and the steps it ran:

{{cite ohao/runtime/frame_log.hpp "struct FrameStamps {"}}

Latency is a movement's poll to the present of that frame. The present is the hard end: a
`vkQueuePresentKHR` that returns has only queued the image. Where the device has
`VK_KHR_present_id` and `VK_KHR_present_wait`, every present carries an id and a `PresentWaiter`
thread asks the presentation engine when that id reached the display, polling with a zero timeout
about every 0.25 ms. Vulkan requires host access to a swapchain to be externally synchronized, so
the poll holds the swapchain's host mutex, which present, rebuild and destroy also take:

{{cite ohao/gpu/vulkan/present_waiter.cpp "std::lock_guard host(m_swapchain->hostMutex());"}}

{{cite ohao/gpu/vulkan/present_waiter.cpp "r = m_wait(m_device, m_swapchain->handle(), p.id, 0);"}}

A present the engine has not confirmed after 250 ms (a hidden window), or any present without
present wait, keeps the stamp of its call and is marked not waited: `source: present_call`, an
underestimate. `render.stats` reports the log's newest frames as `latency` (median, p95 and each
stage's median) and `pacing` (frame time median, p95, p99 and how many frames ran 0, 1, 2 or 3+
steps):

{{cite ohao/api/session.cpp "out.key("latency");"}}

The stamps also showed where the latency was. Late input sampling bounds it: a realtime frame polls
only once at most `LiveConfig::framesAhead` submitted frames are unpresented, so the input goes into
the very next frame instead of waiting behind a queue:

{{cite ohao/runtime/frame_log.hpp "return submitted - static_cast<uint64_t>(framesAhead);"}}

{{cite ohao/runtime/live_host.cpp "m_renderer->waitForPresentOf(wait, kFramesAheadTimeout);"}}

Automatic (-1) is one frame ahead while a player leads its look, unbounded otherwise:

{{cite ohao/runtime/live_host.cpp "return physics && physics->leadsLook() ? 1 : -1;   // a play window keeps one frame ahead"}}

`feel_bench` plays the play scene for a scripted minute per present mode in a visible window while a
mouse movement is fed every 40 to 60 ms. With the lead and late sampling the median look latency
fell from 32.14 ms to 4.80 ms (immediate; budget 15 ms, a pass). Frame p99 over median measured
1.99 against a budget of 1.5, a miss on the record, and `feel_bench` counts the 330 to 500 ms
presentation stalls every window on this laptop shows.

## The gun is drawn, not placed

A first-person gun placed in the world at arm's length goes through a wall the player stands
against, and through the scene's 75 degree lens it looks stretched. A `ViewModel` on the gun's root,
below the entity that holds the view's `Camera`, has its subtree drawn remapped in view space by
`viewModelRemap`:

{{cite ohao/ecs/view_model.cpp "const float k = std::tan(glm::radians(sceneFovDeg) * 0.5f) / std::tan(glm::radians(fovDeg) * 0.5f);"}}

{{cite ohao/ecs/view_model.cpp "s[2][2] = depthScale / k;"}}

The remap is $S \cdot \text{view}$ with a scale in view space, where $d$ is `depthScale` (0.55),
$\theta_s$ the scene's field of view and $\theta_g$ the gun's `fovDeg` (60):

$$S = \operatorname{diag}\!\left(d,\ d,\ \frac{d}{k}\right), \qquad k = \frac{\tan(\theta_s/2)}{\tan(\theta_g/2)}$$

A view-space point $(x, y, z)$ goes to $(dx, dy, dz/k)$. Its screen coordinate through the scene's
lens is $\frac{dx}{-dz/k} \cdot \frac{1}{\tan(\theta_s/2)} = \frac{x}{-z} \cdot \frac{1}{\tan(\theta_g/2)}$:
exactly where the unremapped point lands through the gun's 60 degrees. With $\theta_s > \theta_g$,
$k > 1$, so every point is at most $d$ of its distance from the eye. `t21_view_model_placement`
checks both over 1,000 points.

## Why 0.55 and not smaller

The spec asked for 0.25, which would keep the gun a quarter of its distance from the eye. It
vanished from every capture. The projection is `glm::perspective` without
`GLM_FORCE_DEPTH_ZERO_TO_ONE`, an OpenGL projection whose depth runs over $[-1, 1]$:

{{cite ohao/render/camera/camera.cpp "projectionMatrix = glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);"}}

At view distance $\delta$, with near plane $n$ and far plane $f$, the depth it writes is

$$z_{\text{ndc}}(\delta) = \frac{f+n}{f-n} - \frac{2fn}{(f-n)\,\delta}, \qquad z_{\text{ndc}} = 0 \iff \delta = \frac{2fn}{f+n} \approx 2n$$

Vulkan clips depth below 0, so everything nearer than about twice the near plane is cut: 0.1 m at the
play scene's 0.05 m. At 0.25 the gun's near end fell inside that. At 0.55 it spans 0.108 to 0.27 m,
past the clip and still inside the 0.3 m capsule radius, so no wall can reach it:

{{cite ohao/ecs/components.hpp "depth buffer's clip at about twice the camera's 0.05 m near plane"}}

{{cite docs/specs/2026-10-27-feel-design.md "fell inside that and the gun vanished from every capture"}}

The depth range itself is renderer work for later; the default changed instead.

## Motion at render rate

The `view_model` sync system runs after `interpolate`, so it sees the look lead's drawn view, and
before the render extract. `installCombat` installs it:

{{cite ohao/ecs/combat.cpp "installViewModel(world);"}}

It moves the gun by the window's frame time, not the step: the view it hangs from is drawn every
frame (the lead and interpolation), so motion advanced only at 60 Hz steps would stutter against it.
The host gives the World that time in realtime, clamped to 0.1 s, and 0 in lockstep:

{{cite ohao/runtime/live_host.cpp "m_world->setPresentationDt(m_time.running() ? std::clamp(frameDt, 0.0f, 0.1f) : 0.0f);"}}

{{cite ohao/ecs/view_model.cpp "const float dt = world.presentationDt();"}}

Sway lags the drawn view's turn (0.08 degrees a degree, at most 3) and returns exponentially; bob is
one cycle per `bobStride` (0.75 m) walked on the ground; kick is 0.03 m back and 2.5 degrees up for
each `shot` event in the combat log whose source is the gun's owner:

{{cite ohao/ecs/view_model.cpp "m.swayYawDeg = std::clamp(m.swayYawDeg - wrapDeg(yaw - gun.lastYawDeg) * vm.swayDegPerDeg,"}}

{{cite ohao/ecs/view_model.cpp "m.bobPhase = std::fmod(m.bobPhase + speed * dt / std::max(vm.bobStride, 0.01f), 1.0f);"}}

{{cite ohao/ecs/view_model.cpp "m.kickBack += n * vm.kickBack;"}}

A frame with no presentation time (lockstep, headless, every capture) draws the rest pose, the remap
alone, and a round's restart resets the motion. The motion is written as `RenderPose` only and held
in the registry's context, so it is never saved, hashed or snapshotted (`t21_view_model_never_in_world`).

## No shadow, no bounce

The gun is drawn shrunk toward the eye, so anything that sees it as world geometry sees it in the
wrong place: a CSM cascade would cast a gun-shaped shadow on the floor, and a GI ray would bounce
light off a gun 20 cm from the camera. The render extract marks every instance under a `ViewModel`:

{{cite ohao/ecs/render_extract.cpp "d.viewModel = inViewModel(e);"}}

The draw carries `kDrawViewModel`, and no cascade lists it:

{{cite shaders/cull/instances.slang "member = inFrustum && (d.info.y & (kDrawEmitter | kDrawViewModel)) == 0u;"}}

Its TLAS instance gets mask `kTlasMaskViewModel` (4). Only the path tracer's camera rays and a
near-mirror's first specular ray use `kRayMaskCamera` (7); shadow, GI and bounce rays use
`kRayMaskScene` (1) and never meet it. The values live in `shaders/shared/layout.json`:

{{cite ohao/render/scene/render_scene.cpp ": (g.flags & kInstanceViewModel) != 0 ? gpu::shared::kTlasMaskViewModel"}}

{{cite shaders/shared/layout.json "TLAS instance mask of a first-person gun's mesh (ViewModel)"}}

It is lit like any surface. `light_emitter_gpu_test`'s gun section holds it: the gun changes no pixel
more than 24 px from its footprint, while the same box placed in the world at its drawn pose does
shadow the room.

## Sparks and sounds from the combat log

A window also turns the combat log into particles and audio, beside the footsteps. `CombatFx`
reads the log by cursor each frame: a round is a muzzle burst at the drawn muzzle (the light below
the gun, so the burst leaves the gun the player sees), a hit is sparks along the normal and a puff
by the surface hit (concrete, metal, wood, default):

{{cite ohao/runtime/combat_fx.cpp "return {ParticleSystem::presetImpactSpark(e.point, e.normal), dustFor(surface, e.point, e.normal)};"}}

It emits only while the timeline runs, and a lockstep capture turns the window's particles off, so a
window capture equals a headless one:

{{cite ohao/runtime/combat_fx.cpp "if (!realtime) continue;"}}

{{cite ohao/runtime/live_host.cpp "if (deferred) deferred->setParticlesDrawn(false);"}}

Audio is one `AudioSystem` capped at 24 SFX voices. A sound over the cap replaces the quietest at the
listener, or is dropped if it would be the quietest, and either way is counted in `voicesDropped`:

{{cite ohao/runtime/game_audio.hpp "constexpr uint32_t kVoiceLimit = 24;"}}

Each kind of sound has its own distance curve, rounds carrying to 60 m and footsteps and impacts to
20 m:

{{cite ohao/runtime/game_audio.cpp "case SoundKind::Gunfire: return {1.0f, 60.0f, 1.0f, false};"}}

A sound an enemy starts gets one occlusion ray from the listener, never one per frame and never for
the player's own. Characters on the way are passed through; anything else is a wall, and the sound
plays `kOccludedGainDb` (-8 dB) quieter through a low-pass at `kOccludedLowPassHz` (1,200 Hz):

{{cite ohao/runtime/game_audio.cpp "if (character == entt::null) return true;"}}

{{cite ohao/audio/audio_system.hpp "constexpr float kOccludedGainDb = -8.0f;"}}

`runtime_tests`' `sp9_t21_particles_from_events`, `sp9_t21_voice_cap`, `sp9_t21_distance_curves` and
`sp9_t21_occlusion_ray` cover this half without a window or a device.

:::why
Each piece here changes what the player sees and nothing the world is. The lead, the gun's remap and
motion, the particles and the sounds are all presentation: `RenderPose`, a renderer's emitter list, an
audio voice. None enters a step, `world.hash`, a file or a snapshot. That is what lets the feel work
land without touching the determinism contract: a lockstep window, a headless capture and an agent's
replay still agree bit for bit, because every one of them draws the step's world at rest.
:::

:::key
A realtime window draws the player turned by the look no step has taken, through the step's own
`applyLook`, so the next step lands where the frame was. `FrameLog` and present wait measure a look's
poll to its present: 4.80 ms median. The gun is a view-space remap `diag(d, d, d/k)` at `d` 0.55
(anything nearer than about twice the near plane is clipped), moved at render rate, in no shadow,
cascade, GI or bounce ray. Particles and sounds come from the combat log, in a realtime window only.
:::

## Contracts

- The look lead, the gun's motion, particles and sounds are presentation only: never `Transform`,
  `world.hash`, a saved file or a snapshot.
- A frame with `presentationDt` 0 (lockstep, headless, every capture) draws the gun at rest and no
  particles.
- A new ray that should not see the gun passes `kRayMaskScene`; a new shadow pass must skip
  `kDrawViewModel` draws, as the CSM cascades do.
- `depthScale` must keep the gun's near end beyond about twice the camera's near plane until the
  projection's depth range is fixed.
