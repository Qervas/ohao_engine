---
module: agent
id: time-input
title: Time and input as data
standard: v2
---

## One time axis

An agent can reproduce a run only if every source of change is counted on one axis. In OHAO that axis is the stepped-frame counter of `ohao::Timeline` (`ohao/core/timeline.hpp`). It holds a mode, a pause flag, the fixed step and the frame number. Input stamps, recording checkpoints and the frame `world.hash` and `time.get` report all count on it. (The log's `frame` is different: the renderer's count of frames started.) The timeline is CPU-only and never reads a clock. The host measures wall time and passes it in, which is why `runtime_tests` can test the time model without a window.

There are two modes. In **lockstep** the world advances only on request (`world.step`, `input.replay`), and wall time is ignored. In **realtime** the timeline returns whole fixed steps from the wall time it is given, as a game runs. When paused, realtime behaves as lockstep. One predicate decides which case applies:

{{cite ohao/core/timeline.hpp "[[nodiscard]] bool running() const noexcept { return m_mode == TimeMode::realtime && !m_paused; }"}}

`world.step` and `input.replay` are `E_STATE` while the timeline is running, because steps from two sources would interleave. Realtime itself is `E_STATE` unless the session's host is a window host. A headless host has no loop between requests to run a clock in.

## Integer nanoseconds and the clamp

The fixed step is kept in integer nanoseconds, because floating seconds drift: 60 frames of 1/60 s must be exactly 60 steps, always. The default is 16,666,667 ns, which the spec notes reads back as exactly `1/60` in float, so `world.step`'s default `dt` did not change when the step moved to integers:

{{cite ohao/core/timeline.hpp "Nanos fixedStep{16'666'667};   // 1/60 s"}}

A frame may run at most five steps. After a stall, the whole steps beyond that are dropped, but the sub-step remainder is kept, so the next frame's interpolation factor stays meaningful:

{{cite ohao/core/timeline.cpp "const Nanos excess = (due - m_cfg.maxSteps) * step;"}}

Pausing, resuming and switching mode reset the accumulator:

{{cite ohao/core/timeline.cpp "if (paused != m_paused) m_clock.reset();"}}

So resuming never catches up on the time spent paused, and never runs a step early. The host reports the steps it actually ran with `stepped(n)`. `world.restore` sets the frame back to the snapshot's, because a restored world carries its own frame number. `sp4_t11_realtime_paused_stepped_reaches_the_lockstep_hash` drives a session through realtime frames of uneven length, a pause, a 17-step `world.step` and a resume. Its hash then equals a pure lockstep session's after the same number of steps, and differs after one more step.

## Input is an InputMap stepped per simulation step

`InputMap` (`ohao/core/input.hpp`) turns raw key and button states into named actions and axes. It is CPU-only and GLFW-free, so ECS systems can read it and the API can inject into it. The `World` owns one and steps it once per simulation step with that step's events, never once per rendered frame. That rule makes realtime and lockstep agree: a press belongs to the step that consumed it, whatever the frame rate. After `beginStep`, `held` means down at the end of the step, `pressed` means it went down during the step, and `released` means it stopped being held during the step. A tap (down and up within one step) is both pressed and released, because the map re-evaluates every action after each event that changes what is held.

Key names are one table, the reflected enum `Key`. Its values are GLFW's key codes, with mouse buttons placed above them:

{{cite ohao/core/input.hpp "MouseLeft = 1000, MouseRight = 1001, MouseMiddle = 1002,"}}

The enum is registered in the catalogue, so a misspelled key in a scene file or a command is `E_ENUM` with a suggestion, from the scene codec, at no extra cost. Names are identifiers, so digits are `Digit0` to `Digit9`. `None` means unbound and is not a key that can be pressed.

## Bindings are a component

The bindings are the `InputActions` singleton, so a scene file carries its controls and an agent edits them like any other component. Three EnTT signals mark the map dirty when the component is constructed, updated or destroyed:

{{cite ohao/ecs/world.cpp "m_registry.on_update<InputActions>().connect<&World::onInputActionsChanged>(this);"}}

The next `World::stepInput` clears the bindings and rebuilds them from the singleton. A world that never had an `InputActions` is never marked dirty, so it keeps bindings made in code. Each step then begins by re-evaluating every action from what is held now, before that step's events are applied. The step-8 record names the bug this fixed: before it, a removed binding left its action held until some key changed.

## The virtual device

Injected input is one more device inside the map, not a fake keyboard. An action is held while any bound key is down or the virtual device holds it:

{{cite ohao/core/input.cpp "bool nowHeld = m_virtualHeld.count(name) != 0;"}}

An injected axis value overrides the axis's keys until it is changed, or released with `value: null`. Mouse entries are deltas, added to the step's mouse delta. Injected keys go through the bindings exactly as real keys do. `pressed`, `held` and `released` therefore mean the same thing whether a human or an agent produced them. A focus loss releases keys, buttons and the virtual device alike, and forgets the last cursor position:

{{cite ohao/core/input.cpp "m_haveMouse = false;   // the cursor may be anywhere on return; no jump"}}

## Every step through one function

`world.step`, `input.replay` and the window host's realtime steps all reach the world through one function:

{{cite ohao/api/input.cpp "void stepOnce(Session::Impl& s, std::span<const InputEvent> deviceEvents, float dt) {"}}

It takes this step's device events first, then the injected events due at this step, in the order they were queued. It writes them to the recording, if one is running, steps the input map, runs the simulate systems with `dt`, and advances the frame. Because there is exactly one path, a recording made while a human played in a window contains the same events, in the same order, that a lockstep replay will feed.

## input.inject

`input.inject {events}` stamps each event with a step relative to the next step to run (`step: 0` is that step) and stores it by absolute step. Each event must name exactly one of `action`, `axis`, `key`, `mouse` or `focusLost`; actions and keys need `down`, axes need `value`. Events stamped for the same step keep their order: within a call they apply in list order, and they come after the same step's events from earlier calls (a stable sort, then a merge). Nothing checks that a name is bound. An action no binding mentions is still a held action that `input.state` can read. There are two caps. A call takes at most 10,000 events, counted from the params schema's `maxItems` before any event is decoded. At most 100,000 events may wait at once:

{{cite ohao/api/params.hpp "inline constexpr std::size_t kMaxQueuedInput = 100'000;"}}

Beyond that the call is `E_STATE`, "step the world first". `world.restore` and `scene.load {replace: true}` drop the queue, because its steps refer to a world that is gone.

## input.record

`input.record {path}` writes JSON Lines under the output directory. The header holds `format`, `version`, `schemaHash`, the last `scene.load`'s path and seed when there was one, `startFrame`, `startHash` (a `world.hash`) and `fixedDt`. After it comes every event that reached the map, with its absolute step: keys by name, or by number when the code has no name; `cursor` for absolute pointer moves; `mouse` for deltas. A step whose length differs from the recording's `fixedDt`, such as a `world.step {dt}` with an explicit `dt`, gets a line first, so the replay runs it at the same length:

{{cite ohao/api/input.cpp "if (dt != s.recording->fixedDt) {   // a step of another length: the replay must know"}}

Every 60 steps the world's hash is written, labelled with the step about to run, so it is the hash of the world *at* that step, before it runs:

{{cite ohao/api/input.cpp "if (s.recording && (s.time->frame() - s.recording->startFrame) % kCheckpointEvery == 0)"}}

`{stop: true}` writes an `{"end", "hash"}` line. Starting a recording releases everything held, as a focus loss does, and so does starting a replay, so the two begin from the same input state. The file is plain data: an agent can read it, trim it or write one by hand.

## input.replay

`input.replay {path, verify = true}` is refused while the timeline is running or while injected events are queued, since either would mix other input into the replayed steps. With `verify`, the world's hash must equal `startHash`; otherwise the call is `E_REPLAY`, telling the agent to load the scene with its seed or restore a snapshot first. The recording's steps then run from the current frame:

{{cite ohao/api/input.cpp "// The recording's steps run from the current frame on: its step numbers only order it."}}

Before each step, the replay applies that step's `dt` line if it has one and compares any checkpoint taken at that step. On the first mismatch it syncs the world and returns `E_REPLAY`, whose `expected` names that step. The world is left exactly there for the agent to inspect. `sp4_t13_a_tampered_recording_fails_at_the_right_step` changes one key at step 70 of a 150-step recording, from `D` to `A`. The replay passes the checkpoint at step 60, then fails at 120, having run exactly 120 steps:

{{cite tests/api/api_tests.cpp "CHECK(r.code() == "E_REPLAY" && r.str("/error/message").find("at step 120") != std::string::npos,"}}

A replay is cancellable and reports progress like `world.step`, and a replay made while recording is recorded like any other steps. `verify: false` replays without hashing.

## FlyController: the first playable system

`FlyController` is an engine-provided component, with a simulate-phase system, that makes a scene playable without host code. It reads the axes and actions it names (`move_forward`, `move_right`, `move_up`, `look`, `fast` by default). While `look` is held, it turns by the mouse delta: yaw about world up and pitch about the local X axis, at `lookDegPerUnit` degrees per unit of delta:

{{cite ohao/ecs/fly_controller_system.cpp "t.rotation = glm::normalize(yaw * t.rotation * pitch);"}}

It then moves by `(front·forward + right·right + up·up) · moveSpeed · (fast ? fastMultiplier : 1) · dt`, with `front = rotation · -Z`. It patches the `Transform` only when it actually moved or turned, so an idle camera writes nothing and uploads nothing. `systems_tests` checks that 60 steps forward move `moveSpeed · dt · 60` along the front. In `helmet_game.scene.json` it sits on the `Camera` entity.

:::why
The input layer had to be data, not callbacks, for two reasons the spec gives. An agent must be able to read and change the controls (they are a component) and to play (injection through the same map a human's keys go through). And a session must be replayable, so every event carries the step that consumed it, and the recording checks itself with world hashes. A replay that drifts says exactly where it drifted.
:::

:::key
`Timeline`: lockstep or realtime, integer-nanosecond steps, at most five per frame, no catching up after a pause. `InputMap` is stepped once per simulation step. Bindings are the `InputActions` component, and key names are a reflected enum. Injection is a virtual device. Every step goes through `stepOnce`. Recordings carry a hash every 60 steps, and a replay fails at the first step whose world differs.
:::

## Contracts

- Read `pressed`/`released` in a simulate system or a `fixedUpdate` hook, never once per rendered frame.
- A system may read only the fixed `dt` as its clock. The nondeterminism lint rejects wall clocks and unseeded randomness in the system sources.
- Recordings compare worlds of one engine build. Their header carries `schemaHash`, and adding a saved component type changes the hash of unchanged scenes.
- A replay needs the recording's start state. Restore the snapshot taken before recording, or load the scene with the recorded seed.
