# Sub-project 9 — Feel: measured latency, a camera at render rate, pacing, and the slice's hand feel

Status: written 2026-10-27 after Frank adopted the post-SP7 roadmap ("Follow this"), with Feel as
the next sub-project. The five rulings that order carried are listed as decided under "Rulings
for Frank"; every other judgment call is listed there as open. It is the binding spec once Frank
accepts it.

Roadmap: row 9 of `docs/ROADMAP.md` ("Feel: latency, view and slice polish"; done when the
camera turns at render rate within the latency budget, frame pacing is gated, and Frank's
hand-test list is closed). Before it: 7 (combat, done 2026-10-27). After it: 10 (animation).

Builds on:
- **Sub-project 1** (`2026-10-02-runtime-shell-design.md`): the window, the swapchain
  (`Swapchain::choosePresentMode`: MAILBOX when `vsync` is off, else FIFO), the fixed-step clock
  (`FixedStepClock`, 1/60 s, at most 5 steps a frame), input as actions.
- **Sub-project 4** (`2026-10-23-agent-control-surface-design.md`): `LiveHost` (poll, step,
  render, present in one loop), `render.stats`, `input.inject`, lockstep and realtime time,
  `check_live.py`, `serve_bench`'s noisy-run rules.
- **Sub-project 2b** (`2026-10-24-renderer-upgrade-design.md`): per-pass GPU timing,
  `render_bench`, `MAX_FRAMES_IN_FLIGHT` = 3, the TLAS masks (`kTlasMaskEmitter`), the G-buffer
  draw flags (`kDrawEmitter`), the deferred renderer's GPU particles (`ParticleSystem`, 65,536
  particles, presets for muzzle flash, impact spark, smoke).
- **Sub-project 6** (`2026-10-26-player-physics-design.md`): the player's look in the step
  (`PlayerState.yawDeg`/`pitchDeg`, `PlayerController.lookDegPerUnit`), the head camera, render
  interpolation (`RenderPose` between the last two step poses at the World's render alpha), the
  footsteps through `AudioSystem` in the window host.
- **Sub-project 7** (`2026-10-27-combat-design.md`): the combat step, its event log, recoil
  (`turnPlayer`), the muzzle light and the stand-in gun under the head, impact marks, combat
  sounds, the HUD (`ecs/combat_hud`, the `hud` pass, the 5×7 font).

## Thesis: the view is presentation, the aim is simulation

What the player sees must answer the mouse on the next frame the display shows; where a round
goes must stay what the fixed step decides. The two are separate: the drawn camera may lead the
simulation by the mouse movement the step has not consumed yet, and that lead is presentation
only (never saved, hashed, snapshotted or seen by a step). The step consumes the same movement
with the same arithmetic, so when it runs, the simulated view lands exactly where the drawn one
already was, with no pop. Everything else in this sub-project (pacing, the gun's motion,
particles, sound) is presentation of the same kind: it reads the world and never feeds back.
Lockstep and headless hosts have no lead, so every capture, golden and determinism test keeps
its bytes.

And latency is measured before it is changed: no feel claim without a number from this machine.

## Goal

On `ohao_game assets/scenes/blockout_play.scene.json`:
- A mouse movement reaches the screen within 15 ms (median, poll to present, at 144 Hz with
  vsync off), and the engine reports the number (`render.stats`, `ohao_game --latency`).
- The camera turns on every presented frame, at the display's rate, not the step's 60 Hz.
- Frames come evenly: the frame time's 99th percentile within 1.5× its median over a minute of
  play.
- The gun moves like a held gun: it sways against the turn, bobs with the walk, kicks back with
  each round and settles; it is drawn with its own field of view and never sinks into a wall.
- A round throws sparks at what it hits and the muzzle spits a flash of particles.
- Sounds fall off with distance by a curve chosen for the slice, never more than a capped number
  play at once, and an enemy behind a wall sounds behind a wall.
- The SP7 leftovers are closed: the crosshair no longer overlaps "ENTER TO RESTART", and Frank's
  hand-test list (below) is answered item by item.

## Scope: in 9 vs later

In 9: latency instrumentation and its report; the presentation-only look lead; present-mode
choice and late input sampling; frame pacing measurement and its gate; procedural gun motion
(sway, bob, kick, settle); the gun's field of view and depth placement (render side only); GPU
particles from combat events in a window; audio distance curves, a voice cap, one occlusion ray
for enemy sounds; the HUD overlap; the RT GI speckles in the blockout's doorway if a fix is cheap
(optional, non-blocking); Frank's hand-test items.

Later: arms and gun animations, ADS, sprint and reload poses (10, animation; this sub-project
gives the gun its render placement and procedural motion only); tracers and lit or soft
particles, forward transparency, projected decals (12, VFX); a mixer graph, reverb zones,
HRTF, more than one occlusion ray, sample-accurate scheduling (11, audio); a 120 Hz simulation
(ruled out: the simulation stays at 60 Hz); input-to-photon measurement with external hardware
(a photodiode or a latency analyser: this sub-project measures to the present the driver
reports); upscaling and other frame-cost work (16).

## Why (what the code does today)

- **Look waits for a step.** `LiveHost::run` polls the window each frame and keeps the events
  until a step consumes them (`m_events`); at 144 Hz most frames run no step, so a mouse
  movement waits up to 16.7 ms for one. The step turns the player
  (`p.yawDeg -= (mouse.x + axis(lookX)) * lookDegPerUnit` in `physics_scene.cpp`).
- **Then interpolation draws it a step late.** In realtime the `interpolate` sync system draws
  the player and head between their last two step poses at the timeline's alpha, so a turn the
  newest step made is fully on screen only once the step after it has run. Mouse to screen is a
  wait for a step, plus one step, plus the frame's render and present: roughly 17 to 33 ms plus
  the GPU frame, before the swapchain's queue.
- **Nothing measures it.** `render.stats` reports GPU and CPU time per pass; there is no
  timestamp on input, present or display.
- **The present queue adds frames.** `vsync` off picks MAILBOX (no IMMEDIATE), the swapchain
  holds `minImageCount + 1` images and the CPU may record up to `MAX_FRAMES_IN_FLIGHT` (3)
  frames ahead; input is polled before the wait for a free frame slot, so a polled movement can
  sit behind queued frames.
- **The gun is a box glued to the head.** It is drawn with the scene's projection (75°, near
  0.05 m), so it pokes into walls when the player stands against one, and it never moves
  relative to the view.
- **The particles exist but nothing emits.** `DeferredRenderer::spawnParticles` and its queue
  are wired into the frame (the `particles` pass, timed, drawn before SSS), but no caller exists.
- **Sounds use miniaudio's default attenuation,** every `playSoundAt` plays with no cap, and a
  wall changes nothing.
- **The won screen's crosshair** is drawn over "ENTER TO RESTART" (`combat_hud.cpp`: the
  crosshair is drawn whenever the player is alive and armed).

## Latency: the definition

A mouse movement's latency is the time from the poll that read it to the present of the first
frame whose drawn camera includes it in full. The stages, each a timestamp on one steady clock:

1. **poll**: `Window::poll` returned the event (the OS and the mouse before it are not
   measured);
2. **consume**: the step that applied it to the player (the authoritative aim);
3. **view**: the frame's scene view was resolved with it in the camera;
4. **submit**: the frame's command buffers were submitted;
5. **present**: the presentation engine reports the image presented: `VK_KHR_present_wait` on
   the image's `VK_KHR_present_id` where the device has both; elsewhere the queue present's
   return plus the frame's fence, reported with `source: "fence"` (an underestimate).

"Includes it in full": with the look lead (below), the first frame resolved after the poll;
without it (today), the first frame whose interpolation's *from* pose is the step that consumed
it (the frame after the next step). The engine reports, per realtime window, the median and
95th percentile over the newest frames that carried look input, and each stage's share.

The target is **15 ms median, poll to present, on a 144 Hz display with vsync off** (Frank's
ruling). On a slower display the numbers are recorded and the gate is `pending`, as a
`render_bench` budget whose step has not come.

## The look lead (presentation-only look)

- The host gives the World the look the steps have not consumed: `World::setPendingLook(vec2)`,
  the sum of the `MouseDelta` events it holds (presentation state, like `setRenderAlpha`; 0 in
  lockstep, paused and headless hosts, and after every step that consumed them).
- A sync system `look_lead` after `interpolate` and before `render_extract` gives the
  input-driven, living player its drawn yaw and the head its drawn pitch: the newest step's
  `PlayerState.yawDeg`/`pitchDeg` turned by the pending look through **the same function the
  step uses** (`applyLook(yaw, pitch, delta, controller)`, extracted from the step, the pitch
  clamp included), and writes them into the player's and head's `RenderPose` (rotation from the
  lead; position still interpolated). The camera's subtree (the gun, the muzzle light) follows.
- When the step consumes the movement, it computes the same yaw and pitch the frame before
  drew: no pop, tested bit for bit.
- Rotation is no longer interpolated for the input-driven player: the drawn rotation is the
  newest step's plus the lead. Recoil (`turnPlayer`) therefore appears at its step; the gun's
  procedural kick (below) carries its motion between steps.
- Gamepad look axes (`look_x`/`look_y` as per-step axes) get no lead in 9; the slice is played
  with a mouse.
- Never in `Transform`, `WorldTransform`, `world.hash`, saved files or snapshots; a dead player,
  an enemy and every non-player entity are drawn as today.

## Present mode and late input sampling

- `PresentTarget` takes a present mode (`fifo`, `mailbox`, `immediate`) instead of a vsync
  flag; `immediate` is chosen where the surface supports it and falls back to `mailbox`, then
  `fifo`, logged once. `ohao_game --present <mode>` and `ohao serve --window --present <mode>`;
  `--vsync` stays as `--present fifo`. The latency gate is measured with `immediate` (vsync
  off, Frank's ruling); `mailbox` and `fifo` are recorded beside it.
- **Late input sampling:** the window host waits for the frame slot its next frame will use
  (that slot's fence; with present wait, the present of the frame before) *before* it polls,
  so a polled movement goes into the very next frame instead of queueing behind recorded ones.
  `LiveConfig::framesAhead` (1 by default in a play window, `MAX_FRAMES_IN_FLIGHT` otherwise)
  bounds how far the CPU may run ahead of presentation.
- Captures, readback and the frame-in-flight rule (nothing a frame renders depends on its slot)
  are unchanged.

## Frame pacing

- The window host records each frame's wall time, steps run, steps discarded by the clock's
  5-step cap, and the present-to-present interval (from present wait where available).
- `render.stats` reports the frame time's median, 95th and 99th percentiles over the newest
  frames, and the frames that ran 0, 1, 2 and more steps.
- Gate: over a 60 s scripted play run (walk, turn, fire bursts, enemies chasing) in the play
  scene at the window's size, **p99 ≤ 1.5 × median**. A spike's cause found in the baseline
  (catch-up steps after a hitch, a shader or pipeline built on first use, an allocation in the
  frame, a sound loaded on first play) is fixed at its source: pre-built, pre-loaded or
  amortised, never hidden by smoothing the clock.

## The gun: placement and procedural motion (render side only)

- `ViewModel` (saved, on the gun's root below the head): `fovDeg` 60, `depthScale` 0.25 (the
  view-space shrink toward the eye), and the motion's parameters: `swayDegPerDeg` 0.08 (lag per
  degree of turn), `swayMaxDeg` 3, `swayReturnPerSec` 8, `bobAmplitude` 0.012 m, `bobStride`
  0.75 m (one cycle per footstep distance walking), `kickBack` 0.03 m and `kickPitchDeg` 2.5
  per round, `kickReturnPerSec` 14. Values are first guesses for Frank's hands; they are data,
  tunable live through `world.apply`.
- **Placement:** the gun's drawn pose is its own pose remapped in view space: scaled toward the
  eye by `depthScale` (so it is never further than a quarter of its distance and cannot reach a
  wall the camera is not already in), and scaled in x and y by `tan(75°/2) / tan(fovDeg/2)`, so
  it is drawn as if through a `fovDeg` lens. Positions stay real positions: lighting's
  reconstruction from depth, motion vectors and ray origins stay consistent, with no second
  projection. Its instances carry a `kDrawViewModel` flag: no CSM cascade draws them, and their
  TLAS mask is in no shadow or GI ray's mask (the gun shades from the world and never shadows
  it, as the light emitters are kept out of shadow rays).
- **Motion:** a sync system `view_model` (after `look_lead`) computes, at render rate, sway (the
  drawn view's turn since the last frame, lagged and returned), bob (a phase advanced by the
  player's horizontal ground speed, `PlayerState.velocity` while `onGround`, over the frame's
  time, one cycle per `bobStride`), and kick (from the combat events of the steps since the last
  frame: back and up per round, returning). Its state (sway, phase, kick) is a runtime holder
  in the registry's context, like the impact marks', and it writes the gun's `RenderPose`. Never
  saved, hashed or snapshotted; lockstep and headless hosts give the rest pose, so captures and
  goldens stay as they are.
- The muzzle light is below the gun and follows its drawn pose; the muzzle position the
  particles use is the light's drawn position.

## Particles from combat (a window's presentation)

- The window host emits through `DeferredRenderer::spawnParticles` from the step's combat events,
  beside the sounds: `shot` → a muzzle burst at the shooter's muzzle along the aim (the
  player's from its drawn muzzle light; an enemy's from its eye), `hit` → impact sparks along
  the hit normal, plus a puff of dust by surface (`concrete`, `metal`, `wood`, `default`
  presets tuned from the existing `IMPACT_SPARK` and `SMOKE`).
- **Realtime only:** particles are emitted only while the timeline runs; a lockstep frame and
  every capture (window or headless) draw none (the queue is dropped and the live particles are
  not drawn in a capture), so `check_live`'s window-against-headless comparisons, the HUD
  golden and every other golden are unchanged.
- `LiveConfig::effects` (default on in `ohao_game`; `--no-fx` off) turns them off.

## Audio quick wins

- **Distance curves:** combat and footstep sounds play with an explicit model per kind
  (`inverse`, `minDistance` 1 m, `maxDistance` 60 m, `rolloff` 1 for gunfire; 0.5 m, 20 m and 1.5
  for footsteps and impacts), set on every `playSoundAt`.
- **Voice cap:** at most 24 SFX voices (`AudioSystem::setVoiceLimit`); a new sound over the cap
  takes the place of the quietest at the listener (distance-attenuated volume), or is dropped if
  it would be the quietest; counted (`voicesDropped`).
- **Occlusion:** a sound from an enemy (its shots and footsteps, never the player's own) casts one
  ray from the listener to the source (`PhysicsScene::raycastFrom`, skipping characters); blocked:
  −8 dB and a low-pass at 1.2 kHz (miniaudio's per-sound low-pass node). One ray per sound
  started, never per frame.
- All of it is in the window host's sound players (`runtime/footsteps`, `runtime/combat_sounds`);
  nothing in the step.

## SP7 leftovers

- **The won screen:** no crosshair or hit marker while the round is won (or the player dead, as
  today); "ENTER TO RESTART" stays where it is.
- **Doorway speckles (optional, non-blocking):** the column of RT GI fireflies at the blockout's
  left wall end is investigated (a capture of the GI buffer at that pixel). If a clamp of new GI
  samples in the temporal pass removes them at no measurable cost, it lands, with the deferred
  goldens it moves looked at and re-baselined in their own commit; if not, it moves to 16 with
  the finding written down.
- **Frank's hand-test notes** (the next section) become acceptance items as he writes them.

## Hand-test acceptance (from Frank)

Provisional: SP7's by-hand checklist (`docs/changes/2026-10-27-combat-step5.md` §7), not yet
played by Frank. Each item is closed with Frank's note, a fix and its test, or a ruling that it
stays. Frank's own items are added here as he plays.

| # | Item (provisional, from SP7 §7) | Frank's note | Status |
|---|---|---|---|
| H1 | Mouse captured while focused; look feels immediate; WASD walk, Shift sprint, Space jump | — | open |
| H2 | Left button fires (automatic, 10 a second, 30 a magazine); R reloads; an empty magazine reloads itself (1.5 s) | — | open |
| H3 | The crosshair opens with sustained fire; the view kicks up and settles when you stop | — | open |
| H4 | Each round: the muzzle flash, a mark where it hit, the impact sound by surface; red ticks when a round hurts an enemy | — | open |
| H5 | Enemies see you, chase and shoot in bursts of three; a red vignette and the hurt sound when they hit; health bottom left | — | open |
| H6 | Kill both: CLEARED, Enter restarts; die: DEAD with 3-2-1, then the round restarts where it began; Enter restarts at any time | — | open |
| H7 | Esc frees the mouse; a click takes it back without firing; `--mute` silences footsteps and weapons | — | open |
| H8 | (Frank) | — | — |

## The agent-facing surface

No new method (the 24 KB `tools/list` cap stays untouched):
- `render.stats` gains, for a window host in realtime, `latency {medianMs, p95Ms, frames,
  source: "present_wait"|"fence", stages: {pollToConsumeMs, pollToViewMs, viewToSubmitMs,
  submitToPresentMs}}` and `pacing {frameMs: {median, p95, p99}, steps: {"0", "1", "2", "3+"},
  discardedSteps}`; absent for headless hosts and lockstep windows (nothing to measure).
- `input.inject` mouse deltas in a realtime window take the same path as the OS's: they are
  stamped at injection and measured the same way (how `check_live` and the bench measure).
- `ohao_game --latency` draws the same numbers in the HUD's top-left corner (the font gains `.`).

## Performance budgets

New `feel_bench` (`tools/bench/feel_bench.cpp`, a visible window on the play scene: the GPU's
present path needs one; budgets in `tests/bench/feel_budgets.json`, the noisy-run rules of
`serve_bench`), each tagged `gatedFrom` its step:
- `look_latency_ms` (step 2): median poll to present of injected mouse deltas, 60 s, `immediate`
  present, at the display's refresh: **≤ 15 ms** (gated on a ≥ 144 Hz display, else `pending`);
  p95 reported.
- `frame_p99_over_median` (step 2): the scripted 60 s play run: **≤ 1.5**.
- Baselines (step 1, no budget): the same numbers before any change, per present mode.
- `view_sync_ms` (step 3, CPU): the `look_lead` and `view_model` sync systems together: ≤ 0.01 ms.
- `particles` pass (step 3, `render_bench`, a new `play_fx_1080p` capture that emits a fixed
  burst of 2,000 particles with a fixed dt; the only capture that draws particles): ≤ 0.1 ms at
  1080p.
- Audio: one occlusion ray ≤ 5 µs (the level ray budget), measured in `physics_bench`'s fixture.

## Determinism

Every simulation result is unchanged: the same scene, seed and inputs give the same steps,
rounds and `world.hash`, in lockstep and realtime, headless and in a window. The lead, the gun's
motion, the particles and the sounds are presentation; lockstep frames and captures have none
of them. A new saved type (`ViewModel`) moves every `world.hash` in the serve and CLI
expectations (its name is hashed); those are re-baselined once with that reason, and no golden
image moves.

## Test plan

Tests are numbered after SP7's (test 18): 19 latency and the lead, 20 pacing and present, 21 the
hand-feel presentation.
- **Test 19 (`runtime_tests`, `level_physics_tests`, `check_live.py`):** `t19_latency_stages_ordered`
  (a frame's stamps are monotonic; a frame without look input carries none);
  `t19_lead_matches_next_step` (the play scene, pending look `(37.5, -12.25)`: the drawn yaw and
  pitch equal, bit for bit, `PlayerState` after the step consumes the same events, the pitch
  clamp included); `t19_lead_never_in_world` (`world.hash`, `Transform`, a saved file and a
  snapshot are the same with any pending look); `t19_no_lead_lockstep_dead_or_enemy`;
  `t19_capture_unchanged` (a lockstep window capture equals the headless one bit for bit with
  pending look held in the window); check_live: a realtime window reports `latency` in
  `render.stats` after injected deltas, and the drawn view turns on the frame after the inject.
- **Test 20 (`runtime_tests`, `check_live.py`):** `t20_present_mode_choice` (immediate, then
  mailbox, then fifo, by what a surface offers); `t20_frames_ahead_bounded` (when a frame polls
  its input, at most `framesAhead` earlier frames are submitted and not yet presented); `t20_pacing_stats`
  (the histogram and percentiles from a scripted frame series); check_live: `pacing` present in
  `render.stats` in realtime, absent in lockstep.
- **Test 21 (`combat_tests` `fx_*`, `runtime_tests`, `engine_tests`):** `t21_view_model_rest_pose`
  (no turn, no walk, no round: the remapped pose only); `t21_sway_lags_and_returns`;
  `t21_bob_follows_distance` (one cycle per 0.75 m walked, none standing or in the air);
  `t21_kick_per_round_returns`; `t21_view_model_placement` (the remap: never further than
  `depthScale` of its distance, and its projection through the scene's 75° equals the gun's
  through `fovDeg`); `t21_view_model_out_of_shadow_rays` (a GPU test: a capture with the gun
  against one without differs only inside the gun's pixels, `engine_tests`);
  `t21_particles_from_events` (shot and hit events give the emits, by surface; none in lockstep
  or a capture); `t21_voice_cap`, `t21_distance_curves`, `t21_occlusion_ray` (a wall between
  listener and enemy: one ray, the low-pass and −8 dB; none for the player's own sounds);
  `t21_won_screen_no_crosshair`.
- Every existing suite and golden unchanged, `check.py --all` green.

## Done criteria

- In `ohao_game` on the play scene, the camera turns on every frame; `look_latency_ms` median
  ≤ 15 ms at 144 Hz with `immediate` present (or recorded and `pending` on a slower display, with
  the measured numbers in the change record); `frame_p99_over_median` ≤ 1.5.
- The gun sways, bobs and kicks, never clips a wall; sparks and dust at hits, a muzzle burst;
  sounds attenuate, cap and occlude.
- Every simulation test, determinism test and golden image unchanged; `check.py --all` green.
- The hand-test acceptance table is closed by Frank's notes.

## Build order

1. **Measure.** The timestamps and their report (`render.stats` `latency` and `pacing`,
   `ohao_game --latency`), present modes and `--present`, `feel_bench` with its baselines, test
   19's stamp tests and test 20's present and pacing tests, a baseline change record with the
   numbers per present mode. Nothing that changes what is drawn.
2. **The camera at render rate and pacing.** `applyLook` extracted, `setPendingLook`, the
   `look_lead` sync system, late input sampling and `framesAhead`, the spikes the baseline found
   fixed at their source; test 19 and 20 complete; `look_latency_ms` and
   `frame_p99_over_median` gated.
3. **Hand feel and close.** `ViewModel`, placement and motion, particles from combat events, the
   audio quick wins, the won screen; the doorway speckles if cheap; test 21; the budgets; look
   shots; Frank's hand test; the change record; the roadmap row.

Each step ends with the engine running, every gate green, a change record
(`docs/changes/2026-10-27-feel-stepN.md`, dated the day it is written) and look shots in
`C:\Users\djmax\ohao_refs\look_shots\sp9_feel\` (`stepN_*.png`; latency and pacing plots for
steps 1 and 2).

## Risks

- **Present wait availability:** `VK_KHR_present_wait`/`present_id` may be missing or report
  differently per driver; the fence fallback underestimates, is labelled, and the gate is then
  `pending`, never passed on it.
- **No 144 Hz display on this machine:** the gate is recorded `pending` with the measured
  numbers; Frank plays on his display.
- **A pop when the step consumes the lead:** only if the lead's arithmetic differs from the
  step's; one shared function and a bit-exact test.
- **Recoil appears at its step:** the drawn view no longer interpolates rotation; the kick is at
  most 0.35° a round, and the gun's kick runs at render rate. If Frank feels the step, the
  recoil's share of the turn can be interpolated separately (open ruling 9).
- **A visible window for the bench:** a hidden window may not present; `feel_bench` opens a
  visible one and is `slow`-labelled, run like the other benches.
- **The view-space remap and RT:** the gun's shrunk pose must stay out of every scene ray's mask,
  or tiny shadows appear in the world near the eye; tested.
- **Particles and determinism:** realtime-only emission is what keeps captures identical; a
  capture that drew live particles would break `check_live`'s comparisons; tested.

## Review focus

- A frame with pending look in which the step consumes only part of it (two steps in one frame,
  events split between them): the lead is the remainder, the drawn view continuous.
- Pitch at the clamp: a lead that would pass 89° stops there exactly as the step will.
- A restart or a teleport (a placement) with pending look: the drawn view jumps with the
  placement and keeps the lead on top, never interpolated across.
- Lockstep after realtime (`time.set`) with look still held by the host: the pending look is
  dropped with the events, the next capture equals headless.
- The gun's motion across a restart: the kick and sway reset with the round, no carry-over.

## As built (step 1)

- **Present modes:** `PresentMode {Fifo, Mailbox, Immediate}` in `gpu/vulkan/present_target.hpp`
  (`presentModeName`, `presentModeFromName`); `PresentTarget::mode` replaced `vsync`;
  `Swapchain::choosePresentMode(modes, want)` falls back immediate → mailbox → fifo (mailbox never
  to immediate) and logs what the surface gave once. `ohao_game --present`, `ohao serve --window
  --present`; `--vsync` is `--present fifo`; the default stays mailbox (ruling 7).
- **Present id and wait:** enabled when the device has both extensions and feature bits (this
  machine: RTX 5070 Laptop, yes). Every window present carries its frame number as its present id.
  `PresentWaiter` (`gpu/vulkan/present_waiter.{hpp,cpp}`) is a thread that polls
  `vkWaitForPresentKHR` with a zero timeout about every 0.25 ms (a high-resolution waitable timer)
  while a present is pending, under `Swapchain::hostMutex()`: the swapchain must be externally
  synchronized for that call, so present, rebuild and destroy take the mutex too. A present the
  engine has not confirmed after 250 ms, or any present without the extensions, keeps the time its
  present call returned (`source: present_call`; corrected from the spec's "fence": waiting on a
  frame's fence from another thread would race its reset). Validation stays clean: the play scene
  600 frames with a resize, 0 messages; check_live's realtime window under validation, 0.
- **Frame log:** `runtime/frame_log.{hpp,cpp}`: `FrameStamps` (frame, start, poll, view, submit,
  present, presentWait, the look movements it shows, frame time, steps, discarded steps),
  `FrameLog` (a ring, 1,024 frames by default, `LiveConfig::frameLogCapacity`), `latency(frames)`,
  `pacing(frames)`, `percentile` (linear between closest ranks; the median as `render.stats` has
  always computed it). Named `FrameLog`, not the plan's `FrameTiming`: `render::FrameTiming`
  already names the GPU pass timings.
- **Stamps:** `LiveHost` stamps poll after the window's events (and a program's, `feedEvents`)
  are read, view after the scene view is applied, submit from `VulkanRenderer::lastSubmitTime()`
  (right after `vkQueueSubmit`), present from the waiter. Look movements are the polls whose
  events held mouse motion and the `input.inject` calls whose mouse events a step applied
  (`Session::lastStepInjectedLook()`, stamped at the call); each is consumed by the step that
  applied it and shown in full, today, by the first frame drawn once the step after it has run.
  Realtime frames only; a lockstep or paused window drops its pending movements with its events.
- **`render.stats`:** `latency {frames, samples, medianMs, p95Ms, source, present, stages}` and
  `pacing {frames, frameMs {median, p95, p99}, steps {"0", "1", "2", "3+"}, discardedSteps}` for a
  realtime window over the newest `frames` (1–120) frames; `present` (the window's present mode) is
  an addition. The method's description changed, so `schemaHash` moved (`e88e580b9f990ac2` →
  `7c1e934a206eeff1`): the serve and CLI expectations re-baselined for that alone.
- **`ohao_game --latency`:** `LAT <median> P95 <p95>  FT <median> P99 <p99>` over the newest 240
  frames, top left on a dark band, amber when the presents are unconfirmed; `/` until there is a
  sample. The HUD font gained `.`.
- **`feel_bench`** (`tools/bench/feel_bench.cpp`, label `bench`, GPU, serial): a visible 1920x1080
  window on the play scene with ohao_game's post effects, 2 s of warm-up, then a scripted minute
  per present mode: forward and back, fire 1 s in 3, a mouse movement every 40–60 ms. The spacing
  follows a golden-ratio sequence: a first run fed one every 50 ms, three 1/60 s steps, so every
  movement met the step clock at the same phase and poll → consume read 15 ms instead of a spread
  around 8. Budgets in `tests/bench/feel_budgets.json` (`currentStep` 1: both pending).

**Baseline** (2026-10-27, `feel_bench`, RTX 5070 Laptop, a 240 Hz display, present wait; the
spec's 144 Hz condition is met, so `look_latency_ms` is gated on this machine from step 2):

| Present | Look latency median / p95 | Stages: poll→consume, poll→view, view→submit, submit→present | Frame time median / p95 / p99 (p99/median) | Steps a frame 0 / 1 / 2 / 3+, discarded |
|---|---|---|---|---|
| immediate | 32.14 / 40.38 ms (1,155 movements) | 8.44, 25.08, 0.25, 6.00 ms | 1.725 / 2.620 / 3.062 ms (1.78) | 36,893 / 3,450 / 0 / 5, 126 |
| mailbox | 32.07 / 42.89 ms (1,110) | 7.89, 25.11, 0.29, 5.27 ms | 2.001 / 3.288 / 5.007 ms (2.50) | 31,069 / 3,301 / 1 / 10, 248 |
| fifo | 53.24 / 64.27 ms (1,117) | 9.03, 24.35, 2.91, 27.45 ms | 4.137 / 9.015 / 10.140 ms (2.45) | 9,933 / 3,330 / 0 / 9, 226 |

What it says for step 2: of immediate's 32 ms, about 8 is the wait for a step, about 17 the step
the interpolation draws behind (poll → view less poll → consume), and about 6 the frames queued
ahead of presentation; the render itself is under a millisecond of CPU. Every mode has a few
hitches of about half a second a minute (the frames that ran 3+ steps, the discarded steps, the
500–780 ms latency outliers); their cause is step 2's to find. fifo settles into two-refresh
frames (8.3 ms) after about 20 s.

## Rulings for Frank

Decided (by Frank adopting the roadmap, 2026-10-27):
1. **Latency budget: 15 ms mouse-to-present, median, at 144 Hz with vsync off;** median and p95
   reported in `render.stats`.
2. **Presentation-only look:** the drawn camera may lead the simulation; the step's aim stays
   authoritative for rounds; the lead is never hashed.
3. **The simulation stays at 60 Hz.**
4. **Cornell's lighting miss** (about 0.40 ms against 0.32) stays open and is not a Feel
   blocker; it goes with 16 and quiet bench windows.
5. **kit2k folds into 15; OpenUSD stays by need after 15; FSR is the first upscaler backend**
   when 16 comes.

Open (judgment calls made here, each conservative; any can be reversed):
6. **p95 is reported, not gated;** a p95 gate (proposal: 20 ms) once the baseline shows the
   spread.
7. **`ohao_game`'s default present mode** stays `mailbox` until the step 1 baseline; step 2
   proposes the default with the numbers.
8. **The gun's field of view (60°), depth scale and motion values** are first guesses, to be
   tuned by Frank's hands; the scene's 75° stays.
9. **Recoil is not interpolated** between steps (see Risks); the gun's kick carries the motion.
10. **No gamepad look lead in 9.**
11. **Particles and sounds are a window's presentation only;** captures (and so goldens and agent
    pictures) show none. An agent sees particles only in a realtime window's capture of the
    frame it presents, which is not compared anywhere.
12. **Audio values** (24 voices, the distance curves, −8 dB and 1.2 kHz behind a wall) are first
    guesses.
13. **The doorway speckles are optional;** they move to 16 if a cheap fix does not hold.
14. Later rows' pillar questions (animation content and licence, rig standard, root motion and
    IK, compute or vertex skinning, determinism of animation, ragdoll and audio, the audio stack,
    the engine/game boundary) stay for those rows' specs. The post-SP7 suggestions recommended
    compute skinning, in-place animation with the controller in charge, and bit-exact
    determinism; those are recommendations, not rulings.
15. **Step 1: the fallback present stamp is the present call's return** (`present_call`), not a
    fence observed from another thread; it is an underestimate, labelled, and never passes a gate.
16. **Step 1: the waiter polls** `vkWaitForPresentKHR` with a zero timeout every ~0.25 ms under the
    swapchain's mutex rather than blocking in it (a blocking wait would hold the mutex the next
    present needs); the stamp's resolution is about 0.25 ms.
17. **Step 1: `feel_bench` has no noise detection** (no calibration, no busy-process probe): its
    budgets are pending in step 1; step 2 adds a probe before they gate.
18. **Step 1: injected look counts from its `input.inject` call,** so agents and check_live
    measure the same latency the device path does (both wait for the next step).
