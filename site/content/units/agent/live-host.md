---
module: agent
id: live-host
title: The live host
standard: v2
---

## One loop for the game and the agent

`runtime::LiveHost` is the window host. It owns a GLFW window, a `VulkanRenderer` presenting into it, the file asset resolver, a `World` with instantiate and the render extract installed, and an `api::Session` on that world. Two programs run it. `ohao_game` plays a scene in realtime with no protocol. `ohao serve --window` runs the same loop in lockstep and pumps a protocol server once per frame, so an agent can drive the engine while a human watches. There is one frame loop in the tree, and anything an agent can do in a window, the game does the same way.

At start-up the session takes the host's `Timeline` instead of its own, and the host as its `api::Host`:

{{cite ohao/runtime/live_host.cpp "m_session->setTimeline(m_time);"}}

The shared timeline means a realtime step the window runs and a `world.step` an agent asks for count on the same frame axis. That axis is the one that input stamps, recordings and logs use. As a `Host` it reports kind `window`. The session therefore allows `time.set {mode: "realtime"}`, which a headless host refuses, and routes `render.capture` to `LiveHost::capture`.

Members are declared so that they are destroyed in the right order. The session goes first, because it holds a `World&`. Then the world, because its systems hold `RenderScene` slots. Then the renderer, and the window last:

{{cite ohao/runtime/live_host.hpp "destroyed before the renderer (it holds RenderScene slots)"}}

`shutdown()` performs the same order explicitly and returns the validation count *including teardown*, because the validation layer reports leaked objects only when the device is destroyed.

## The frame, in order

Each loop iteration does six things, always in the same order. First it polls the window. Events are appended to a list that survives the frame, because a realtime frame shorter than the fixed step runs no step, and the next step must still see those events:

{{cite ohao/runtime/live_host.cpp "m_events.insert(m_events.end(), polled.begin(), polled.end());   // kept until a step consumes them"}}

In a play window (`setClickToCapture(true)`, which `ohao_game` sets when the scene has a player) the newly polled events first pass through `ClickToCapture`: a mouse press while the cursor is free, and its release, are dropped and only ask for the cursor, so the click that takes the mouse back never fires a round:

{{cite ohao/runtime/live_host.cpp "if (m_clickToCapture && m_clickCapture.filter(polled, m_window.cursorCaptured())) m_captureClicked = true;"}}

Second, it pumps requests. `RequestPump` is two callbacks, `poll` and `flush`, which `serveWindow` binds to `rpc::Server`. `poll` runs everything queued, on this thread, at this point in the frame. While the clock is not running, it waits up to 16 ms for the first message. That keeps an idle lockstep window from spinning a core, while it still services the OS event queue about 60 times a second:

{{cite ohao/runtime/live_host.cpp "if (pump && pump->poll && !pump->poll(std::chrono::milliseconds(m_time.running() ? 0 : 16))) m_quit = true;"}}

When the server's input ends (stdin closed, or the pipe's client gone), `poll` returns false and the loop ends after this frame.

Third come the timeline's steps. `Timeline::advance` is given the wall time since the last frame and returns the fixed steps due: whole steps in realtime, none in lockstep or while paused. Each step is one `Session::step(events, fixedDt)` followed by the game's `fixedUpdate` hook. `Session::step` feeds that step's device events and injected events to the world's input map and runs the simulate systems (see the time and input unit). The spec's "before the step" slot is gone, because a step is now a single call. Fourth, the `update` hook runs once per frame. Fifth, the host renders, if a frame is due. Sixth, it flushes the replies.

## Input that reaches no step is dropped

In lockstep, the world moves only when an agent asks. If window events kept accumulating between requests, the next `world.step` would replay whatever keys the human had touched in the meantime, and a lockstep run would stop being reproducible. So while the clock is not running, the host throws the window's events away:

{{cite ohao/runtime/live_host.cpp "if (!m_time.running()) {   // lockstep or paused: the window's input reaches no step"}}

The same branch sets `m_releaseOnResume`. When stepping resumes, the first step starts with a focus loss:

{{cite ohao/runtime/live_host.cpp "m_events.insert(m_events.begin(), InputEvent{InputEvent::Type::FocusLost});"}}

`FocusLost` releases every held key, mouse button and injected action or axis. Without it, a key held when the human paused and released during the pause would never get its key-up event (that event was dropped), and the action would stay held forever. A lockstep world therefore sees input only through `input.inject`, and a realtime world resumes from nothing held.

## Render only when something may have changed

In realtime every frame is rendered. In lockstep or paused, rendering an unchanged world would draw the same image again for no reason, and keep the GPU busy while an agent is thinking. A frame is due when the clock runs, when a resize or the first frame asks for one, or when the session's change count has moved since the last render. One exception keeps drawing an unchanged world: a window presenting `rt_offline` (`--mode rt_offline`) accumulates one sample pass a frame until `--spp` samples, so a human watches the image converge, then stops:

{{cite ohao/runtime/live_host.cpp "const bool converging = m_liveMode == RenderMode::RTOffline && m_liveSamples < m_cfg.spp;"}}

`Session::changeCount` counts `Session::step` calls and calls to the methods marked as mutating: `scene.load`, `world.apply`, `world.step`, `world.restore` and `input.replay`. It counts them whatever their outcome:

{{cite ohao/api/session.cpp "if (method.mutates) ++m.changes;   // counted whatever the outcome: a cancelled step moved the world too"}}

A cancelled step did move the world, so it must count. A failed batch rolled back and counts anyway; the cost is one extra frame. Queries, hashes, snapshots and log reads do not count, so an agent orienting itself with `world_summary` and `world_query` causes no rendering at all. `sp4_t16_change_count_rises_only_for_methods_that_change_the_world` pins the list.

A minimized window has a 0×0 framebuffer. The host still simulates, but does not render:

{{cite ohao/runtime/live_host.cpp "if (ext.width != 0 && ext.height != 0 && due) {   // minimized: simulate, don't render"}}

Rendering a frame starts with `World::sync()`, then applies the world's `Camera` and `Environment` to the renderer (`applySceneView`, shared with the headless host and the examples), then calls `render()`:

{{cite ohao/runtime/live_host.cpp "m_world->sync();   // instantiate, transforms, the render extract: never a simulation step"}}

## Replies after the frame they caused

The last thing in the frame is the flush:

{{cite ohao/runtime/live_host.cpp "if (pump && pump->flush) pump->flush();   // replies after the frame they caused"}}

`serveWindow` creates the server with `holdReplies`, so the replies to requests run during this frame's poll wait until the frame has been presented:

{{cite tools/ohao/main.cpp "options.holdReplies = true;   // sent after the frame the requests caused is presented"}}

When the agent reads the reply to `world_apply`, the window already shows the edit. An agent that captures the screen by some other means, or a human watching, never sees an acknowledged change that is not yet on screen. Progress and log notifications are not held.

## Captures inside the poll

`render.capture` runs synchronously inside the poll, on the engine thread, like any other request. The spec had planned to move the encode off-thread. As built it stays inline: it takes a few milliseconds, and the reply has to wait for it anyway. A lockstep capture runs `runtime::captureFrame`, the same procedure as the headless host (see the capture unit), saving the window's own post settings first and restoring them afterwards. The capture's last frame was presented and shows the current world, so the host records the change count as rendered:

{{cite ohao/runtime/live_host.cpp "// The capture's last frame was presented and shows the current world: no frame"}}

Without that, the loop would render one more frame right after the capture. That frame would still be in flight when the next capture's temporal reset idles the device, and would delay it. A realtime capture returns the next presented frame as it is, and is marked `deterministic: false`.

## The game is a scene file

`ohao_game` has no game code that moves anything. It loads `assets/scenes/helmet_game.scene.json` through the session: the helmet room, a cube carrying `Spin`, a camera carrying `FlyController`, and an `InputActions` entity binding WASD, Q/E, the right mouse button to look, Shift and Escape. The program then sets the host's render settings (post effects on, exposure 1), which a scene file does not hold, and installs two hooks. The quit check reads the input in `fixedUpdate`, because input is stepped per simulation step, and a press read once per frame would be lost when two steps run in one frame:

{{cite examples/ohao_game.cpp "if (h.input().pressed("quit")) h.quit();"}}

The `update` hook captures the cursor while `look` is held. With `--frames N` the run is the presentation gate. It fails if any frame did a GPU-to-CPU copy nobody asked for, or if validation (with `OHAO_VALIDATION=1`) reported anything, teardown included:

{{cite examples/ohao_game.cpp "if (r.readbackCopies != 0) return 3;"}}

`OHAO_VALIDATION=1 ohao_game --frames 600 --resize-at 200` is in the gate list. `check_live.py` drives `ohao serve --window --hidden` end to end. A hidden window is never shown but still presents. That run covers load, apply, step with watch, inject, captures, a lockstep window capture equal to a headless one, `E_STATE` when minimized, a disconnect in the middle of a step, and the pipe.

:::why
`ohao serve --window` defaults to lockstep, although the same loop runs the game in realtime, because the spec's determinism contract covers lockstep sessions: the same scene, seed, method sequence and injected input give bit-identical world state and identical replies. A realtime session is not reproducible as such (only its recording is), and its world moves during the agent's round trip, so a query result is stale before it arrives. The human watching loses nothing: the window keeps showing its last frame, and `time.set` switches to realtime when the agent wants to play.
:::

:::key
Each frame: poll the window; run the queued requests (waiting up to 16 ms while nothing steps); run the timeline's steps through `Session::step`; render when the clock runs or the change count moved; flush the replies after present. Device input reaches only steps that run, and stepping resumes with a focus loss.
:::

## Contracts

- Everything that touches the world runs on the loop's thread: hooks, requests and captures.
- A game reads `pressed`/`released` in `fixedUpdate`, never in `update`.
- A method that changes the world must be marked `mutates` in the dispatch table, or a lockstep window will not show its effect until something else renders.
- The session must be destroyed before the world, and the world before the renderer. `shutdown()` does this; a host that tears down by hand must keep the order.
