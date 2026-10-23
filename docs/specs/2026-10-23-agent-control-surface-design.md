# Sub-project 4 — Agent control surface and agent dev ergonomics

Status: written 2026-10-23 on Frank's go-ahead ("sub-project 3 is done; go ahead with
sub-project 4"). Frank delegated the technical and language choices; this document makes
them. It is the binding spec once Frank accepts it. Nothing is left open; the few things
only Frank can do are listed under "Frank actions".

Roadmap: row 4 of `docs/ROADMAP.md`. After 4 comes 2b (GPU-driven rendering + renderer
upgrade), then the vertical slice (5–7).

Builds on sub-project 3 (`docs/specs/2026-10-22-reflection-scene-serialization-design.md`,
"spec 3" below): the type registry, the scene format, the `ohao/api` method table, the
`ohao` CLI and the determinism contract. Nothing here changes their semantics; 4 adds a
transport, a live host, time and input control, capture, logs, and the tooling an agent
needs to build and verify OHAO from one command.

## Thesis: an AI-native engine, continued

Spec 3 made the agent surface the foundation: typed commands with undo, canonical scene
files, a schema, deterministic headless runs, one protocol. An agent can author a scene
and render it, but only one process per call, only headless, and only as a still: it
cannot watch the engine run, play it, or read what the engine says while it runs. And an
agent working *on* OHAO still assembles its verdict from a dozen commands and free-form
text.

Sub-project 4 closes both gaps, for the same two users as before:
- **G1, agents build games with OHAO:** a live engine an agent drives over a standard
  protocol (MCP), with the same methods a human editor will use.
- **G2, agents develop OHAO:** one command that builds and verifies everything and answers
  with machine-readable pass/fail, plus diagnostics an agent can read without a human.

## Goal

`ohao serve` runs the engine as a JSON-RPC 2.0 server on stdio that MCP clients connect to
natively. It exposes the whole spec-3 method table plus time control, input injection,
recording and replay, live frame capture and log streaming. It runs headless or with a
window, where a human can watch (and play) while the agent works.

`python tools/check.py` configures, builds (shaders first), checks the environment, runs
every suite under CTest and writes one JSON verdict plus JUnit XML.

**Done when:**
- An MCP client connected to `ohao serve --window` loads a scene, edits it, plays it with
  injected input, captures frames and reads logs, with no human step. A scripted MCP
  client does this in the test suite, and a real agent (Claude Code through the repo's
  `.mcp.json`) does it once in the acceptance run recorded in the change record.
- In lockstep, the same session gives bit-identical world state and bit-identical deferred
  captures run to run, and an input recording replays to the same world hash at every
  checkpoint.
- `python tools/check.py --all` builds OHAO and runs every suite, golden, validation run,
  doc check and benchmark, and exits 0 with `build/check/check.json` reporting every test.
  A deliberately failing test makes it exit 1 and names the failure with its output.
- Every budget in "Performance" passes on the reference machine, five consecutive runs.

## Agents as users, in 4

G1 adds five properties to spec 3's five:

6. **Live and long-lived.** One engine process serves a whole session: no start-up per
   operation, the renderer stays warm, snapshots and recordings persist.
7. **Playable.** Time is the agent's to control (lockstep, pause, step, realtime), and
   input is data: actions defined in the scene, injected by name, stamped by simulation
   step, recorded and replayed.
8. **Observable at low cost.** Captures come back as compact images an agent can look at,
   state comes back projected (`world.step` can return the fields it watches), changes
   come back as diffs, and logs come back filtered and capped.
9. **Standard protocol.** MCP over stdio, the transport agents already speak. Every API
   method is an MCP tool whose input schema is generated from the same registry as the
   scene schema.
10. **Safe by default.** stdio only; no listener of any kind unless asked for, and then a
    local named pipe for the current user only. File access is confined to the project
    root (reads) and an output directory (writes).

G2 adds: one build-and-verify entry point with JSON and JUnit results, machine-readable
output from every suite and harness, golden diffs an agent can inspect (JSON regions and
heatmap images), `ohao doctor` for the environment, and the recipes an agent needs to
extend the renderer.

## Scope: in 4 vs later

| In sub-project 4 | Later (designed for here, built in its own sub-project) |
|---|---|
| `ohao_rpc`: JSON-RPC 2.0 over stdio (newline-delimited), MCP server adapter, cancellation, progress, log notifications | Network transports (TCP, WebSocket) with authentication; multi-client sessions |
| `ohao serve`: headless and windowed hosts | The editor UI (an in-process `ohao/api` client) |
| Method params as reflected structs: one definition gives validation, the catalogue and MCP input schemas | C ABI `ohao_c.h`; zero-copy observation views |
| Time model: simulate/sync system phases, lockstep and realtime clocks, pause/step | Physics in the ECS (sub-project 6) |
| Input as data: `InputActions` component, key names, injection, record/replay | Gamepads, text input, touch |
| `FlyController`, the first input-driven engine-provided system | The player controller (sub-project 6), gameplay code and hot reload |
| Live frame capture (lockstep: deterministic; realtime: the presented frame) | Video capture, AOV capture over the protocol |
| `world.hash`, `world.diff` | `ohao diff` between files, cooked formats |
| `ohao/core/log` with a ring buffer and sinks; every engine print migrated | Log files with rotation, tracing/profiler streams |
| Deterministic deferred rendering (the SSAO seed) | RT determinism (RT offline flips, `rt_realtime`): 2b |
| `tools/check.py`, CTest labels, shared harness JSON, golden diff JSON + heatmaps, `ohao doctor`, recipes | CI on a hosted runner |
| `serve_bench` and its budgets | A GPU frame-time gate (2b's render bench) |
| `ohao_game` built on a scene file and the live host | A launcher, packaging |

## Why (what the current code does)

Each point is a fact about the tree on 2026-10-23.

- **The API answers one call at a time, in-process.** `api::Session::call(method, params)`
  (`ohao/api/session.hpp:60`) is the one dispatch. The only way to reach it from outside is
  `ohao run --script`, one process per script; each render pays renderer start-up again
  (481 ms median Cornell load-to-first-frame, spec 3 step 9).
- **Method params have no machine schema.** The method table holds params as doc strings
  (`ohao/api/session.cpp:16-45`, for example `"{frames = 1, dt = 1/60}"`) and each method
  checks its keys by hand (`checkKeys`). An MCP tool needs a JSON Schema per method.
- **The windowed loop runs on wall time.** `runtime::Application::run`
  (`ohao/runtime/application.cpp:24-60`) advances a fixed-step clock from `steady_clock`,
  runs the fixed-update hooks, then runs every World system once with the *frame* dt
  (`m_world->update(frameDt)`, line 49). The spin system therefore turns by wall time, and
  nothing in the window is reproducible.
- **Systems have one phase.** `World::update(dt)` runs `spin`, `instantiate`, `transform`
  and `render_extract` in one list (`ohao/ecs/world.cpp:53-56`). Headless capture calls
  `update(0.0f)` per frame just to sync (`tools/ohao/render_host.cpp:101`).
- **Input is code.** `ohao_game` binds actions to GLFW numbers in C++
  (`examples/ohao_game.cpp:145-150`) and flies the camera from a hook, not a system.
  `InputMap` (`ohao/runtime/input.hpp`) is GLFW-free but lives in `ohao_runtime`, which the
  ECS cannot depend on. There is no way to inject, record or replay input.
- **`ohao_game` is not built from a scene file.** It spawns its scene in code and sets the
  camera on the renderer (`examples/ohao_game.cpp:53-124`).
- **Capture exists only headless.** `RenderHost::capture` renders N frames and reads back
  `getPixelSpan()`. The windowed renderer does no readback at all (the presentation gate
  requires `readbackCopyCount() == 0`), and the AOV readback helpers stall the whole queue
  (`vkQueueWaitIdle`, `ohao/gpu/vulkan/renderer.cpp:903` and the like).
- **There is no logger.** `ohao/` writes 408 times to `std::cout`/`std::cerr`/`printf`
  across 54 files. The CLI keeps its stdout clean by pointing descriptor 1 at stderr
  (`tools/ohao/main.cpp:97-117`). A server whose stdout is the protocol needs that and more.
- **Deferred post effects are not reproducible across processes.** `SSAOPass` seeds its
  4x4 noise texture from `std::random_device` (`ohao/render/deferred/ssao_pass.cpp:260`).
  That fits every symptom seen in 3: two fresh renderers differ by 1 LSB with TAA, SSAO and
  bloom on (`restore_render_gpu_test`), the helmet deferred golden (which turns SSAO on)
  moves on ~1.7% of pixels run to run, Cornell deferred (no post) is exact, and one
  `restore_render_gpu_test` run failed without the failure being captured.
  - *Corrected in step 1:* the seed was real per-process randomness, but it was not the
    cause. `deferred_lighting.frag` never samples the SSAO output (the lighting pass binds
    it at binding 10, which the shader does not declare), so SSAO reaches no pixel. A
    pass-by-pass bisection (each effect alone) found two causes: bloom's render passes had
    no dependency making the previous pass's writes visible to their sampling (a
    read-after-write race down and up the mip chain), and TAA's first frame took its
    motion from `DeferredRenderer::m_prevViewProj`, a `glm::mat4` that a fresh renderer
    never set. See "Determinism", as built.
- **CTest is inert.** Ten suites call `add_test`, but nothing calls `enable_testing()`, so
  `ctest` finds no tests. `tests/CMakeLists.txt` (a gtest `engine_tests`) is included by
  nothing and is dead.
- **Results are partly machine-readable.** `reflect_tests`, `scene_tests`, `api_tests`,
  `systems_tests`, `scene_bench` and the Python harnesses print the one-line JSON summary;
  `engine_tests`, `ecs_tests`, `runtime_tests`, the physics, audio and force suites, the
  small renderer tests and the `*_gpu_test`s do not.
- **The golden harness says pass or fail and little else.** `render_golden.py` prints
  max_abs and diff fraction; an agent cannot see *where* an image changed.
- **The verify recipe is 30 commands.** The `verify` skill runs them one by one; there is
  no single command, and a stale-SPIR-V build can still pass the CPU suites.

## Architecture

```
ohao/core      +  ohao_core      log (levels, channels, ring, sinks); input (InputMap, key names)
                                 moved here from ohao/runtime so the ECS can read input.
ohao/ecs       +  ohao_ecs       system phases (simulate | sync), InputActions, FlyController,
                                 world hash.
ohao/api       +  ohao_api       method params as reflected structs; new methods (time, input,
                                 log, hash, diff, engine.info); the Host interface.
ohao/rpc       NEW ohao_rpc      JSON-RPC 2.0 framing, MCP adapter, transports (stdio, named
                                 pipe). CPU-only, no Vulkan, no window.
ohao/runtime   +  ohao_runtime   Application drives the World by phases; LiveHost.
tools/ohao     +  ohao.exe       `serve`, `doctor`; ohao_host gains the live capture path.
tools/bench    +  serve_bench    RPC, step, capture and log budgets.
tools/check.py NEW               the one build-and-verify entry point (test-only Python).
```

**Dependency rules, tested** (extending spec 3's):
- `ohao_rpc` depends on `ohao_api` and `ohao_core` only. It never includes Vulkan, GLFW or
  the renderer; a host hands it a `Session`.
- `ohao_api` stays GPU- and window-free: everything a host owns (rendering, the clock in
  realtime mode, the window) is reached through the `api::Host` interface below.
- Simulate-phase systems may not include `reflect/`, `api/` or `rpc/` (spec 3's rule) and
  may not read a wall clock or unseeded randomness (see "Determinism").

### The host interface

`api::Session` keeps the dispatch. Spec 3's single `RenderHandler` becomes an interface a
host implements, so the headless CLI, the headless server and the window host share one
session:

```cpp
// ohao/api/host.hpp (shape, not final code)
class Host {
public:
    virtual ~Host() = default;
    virtual HostKind kind() const = 0;                                  // cli | headless | window
    virtual RenderOutcome capture(const CaptureRequest&) = 0;           // render.capture
    virtual TimeState time() const = 0;                                 // time.get
    virtual Result<TimeState, reflect::Diagnostic> setTime(const TimeRequest&) = 0;
    virtual void beforeSteps(int frames) {}                             // window host: input stamping
};
```

`Session::setHost(Host*)` replaces `setRenderHandler`. A session without a host is the
spec-3 CPU session: `render.capture` is `E_COMMAND`, time is lockstep only.

### Method params are reflected structs

Every method's params become a registered struct, for example:

```cpp
struct WorldStepParams { int32_t frames{1}; std::optional<float> dt; std::optional<WatchSpec> watch; };
r.type<WorldStepParams>("WorldStepParams", 1).doc("world.step params")
    .field("frames", &WorldStepParams::frames).doc("Fixed steps to run").min(0).max(1'000'000)
    .field("dt", &WorldStepParams::dt).doc("Step length; default the session's fixed dt").unit("s").min(0.0f)
    .field("watch", &WorldStepParams::watch).doc("Fields to return after stepping");
```

One definition then gives:
- validation of params, with the same typed decoders, limits, `E_UNKNOWN_FIELD` and
  suggestions as scene files (the hand-written `checkKeys` calls go);
- the method table in the catalogue: params as a JSON Schema instead of a doc string
  (`schemaHash` changes once, in step 5);
- the MCP `inputSchema` of each tool.

Params that are free JSON by nature (`world.apply`'s `commands`, `scene.load`'s `scene`)
stay free JSON inside the struct (a `JsonValue` field kind, written as `{}` in the schema
with a doc string), and are checked by the existing command and scene decoders.

## Protocol

### Transport and framing

- **stdio, newline-delimited JSON.** Each message is one compact UTF-8 JSON object on one
  line, ended by `\n`; `\r\n` is accepted on input. This is MCP's stdio transport, and raw
  JSON-RPC clients use the same framing.
  - *Correction to spec 3:* spec 3 (decision 20, "Future transport") said LSP-style
    `Content-Length` framing. MCP's stdio transport is newline-delimited, and compact JSON
    never contains a raw newline, so OHAO uses newline framing and no headers. Spec 3
    carries a note pointing here.
- **stdout carries protocol messages only.** The server writes messages through a private
  copy of the real stdout (spec 3's CLI technique) and points descriptor 1 at stderr, so
  third-party prints (Jolt, NRD, assimp, the driver) cannot corrupt the stream. Logs go to
  stderr and, on request, to the client as notifications.
- **Limits:** a message is at most 64 MiB (`--max-message`); longer input is answered with
  `-32600` and the line is discarded. No JSON-RPC batches (arrays are `-32600`): MCP
  dropped them in 2025-06-18, and `world.apply` already batches commands atomically.
- **Named pipe, opt-in:** `--pipe NAME` serves `\\.\pipe\ohao-NAME` instead of stdio (see
  "Security"). Same framing. One client at a time.
- **No other transport in 4.** There is no TCP or WebSocket listener, not even behind a
  flag.

### Messages, ordering, cancellation

- Requests, notifications and responses as JSON-RPC 2.0. An `id` (number or string) is
  echoed back byte for byte, so 64-bit integers survive.
- **One engine thread, strict order.** A reader thread parses lines and queues requests;
  the engine thread executes them one at a time, in arrival order, at a defined point of
  the frame (window host) or immediately (headless). A long request (`world.step` with
  10,000 frames) delays the ones behind it; that is the price of determinism, and the
  agent can cancel.
- **Cancellation:** MCP's `notifications/cancelled {requestId}` works in both modes. The
  reader thread sets a flag that `world.step`, `input.replay` and multi-frame captures
  check between frames. A cancelled request gets no response (MCP's rule); the world is
  left at the last whole step, and the next `time.get` reports where.
- **Progress:** a request whose params carry MCP's `_meta.progressToken` gets
  `notifications/progress` every 250 ms during `world.step`, `input.replay` and captures.
- **Answered on the reader thread:** `ping` and `notifications/cancelled`, so a busy engine
  still answers liveness checks. Every other message waits its turn.
- **Disconnect:** EOF on stdin (or the pipe closing) cancels the running request, finishes
  the frame, tears down cleanly (validation counted through teardown) and exits 0.

### Errors

Domain errors keep spec 3's object; the JSON-RPC envelope gets integer codes:

| Situation | JSON-RPC `error.code` | `error.data` |
|---|---|---|
| Unparseable line | -32700 | — |
| Not a request object, batch, oversize | -32600 | — |
| Unknown method (`E_METHOD`) | -32601 | `{code: "E_METHOD", suggestion?}` |
| Params that fail the params struct (`E_UNKNOWN_FIELD`, `E_TYPE`, `E_RANGE`, …) | -32602 | spec-3 diagnostics |
| Any other OHAO error | 1, 2 or 3: the CLI exit class (validation/usage, asset, GPU/internal) | `{code: "E_…", diagnostics: [...]}` |

`error.message` is the first diagnostic's message with "(+N more)", as in spec 3. Over MCP,
domain errors are tool results with `isError: true` (so the model sees them), and only
protocol errors (-32700, -32600, -32601 for an unknown tool, -32602) are JSON-RPC errors.

New error codes: `E_STATE` (a method not allowed in the current time mode or host, for
example `world.step` while realtime is running), `E_ACCESS` (a path outside the sandbox),
`E_REPLAY` (a recording that does not match the world it is replayed on).

### MCP mapping

- **Handshake.** `initialize` negotiates the MCP protocol version: the server supports
  `2025-06-18` and `2025-11-25` and answers with the client's version if supported, else
  `2025-11-25`. It declares `capabilities: {tools: {}, logging: {}}`, `serverInfo: {name:
  "ohao", version}` and an `instructions` string (at most 600 characters: orient with
  `world_summary`, read with projected `world_query`, edit with `world_apply`, step with
  `watch`, look with `render_capture`). Native clients skip it and call `engine.info`.
- **Tools.** One tool per method. The name is the method with `.` replaced by `_`
  (`world.query` → `world_query`): Claude's tool-name pattern rejects dots, and the mapping
  is mechanical both ways. The description is the method's doc line. `inputSchema` is the
  params struct's JSON Schema (draft 2020-12, `additionalProperties: false`, defaults
  given).
- **Results.** A tool result is one `text` content block holding the compact `result`
  JSON. No `structuredContent` and no `outputSchema`: MCP asks servers that send structured
  content to send the same JSON as text too, which would double every reply's tokens.
  Captures add an `image` content block (see "Frame capture").
- **Logging.** `logging/setLevel {level}` turns on `notifications/message {level, logger:
  channel, data: text}` at that level and above. Off by default: a client that never asks
  pays nothing.
- **Not used in 4:** resources, prompts, sampling, elicitation, roots. The catalogue is
  reached through `schema_get`.
- **Size budget.** `tools/list` is gated at ≤ 24 KB (≈ 6k tokens; a test measures it), so
  connecting costs an agent a bounded, known amount.

### Method table

Spec 3's methods keep their params and results; additions are marked. New methods are in
the second table. "Window" means the window host.

| Method | Change in 4 |
|---|---|
| `scene.load` | `replace = false`: when true, clears the world first (snapshots are kept). Spec 3 required an empty world, so an agent could not reload without restarting. |
| `world.step` | `dt` defaults to the session's fixed dt. `watch: {ids?, names?, fields}` returns those fields after the last step (one round trip instead of two). Window host: allowed in lockstep or paused, else `E_STATE`. |
| `render.capture` | Becomes host-dependent (see "Frame capture"); gains `inline`, `maxSize`, `format`; reports `deterministic`. |
| `world.info` | Adds `host` and the time state. |

| New method | Params → result |
|---|---|
| `engine.info` | → `{protocolVersion: 1, schemaHash, version, host, gpu?, time}`. The native handshake. OHAO's `protocolVersion` stays 1: every change in 4 is additive. |
| `time.get` | → `{mode: lockstep \| realtime, paused, frame, fixedDt, fps?}` |
| `time.set` | `{mode?, paused?, fixedDt?}` → as `time.get`. Headless hosts: lockstep only. |
| `world.hash` | → `{hash, frame}`: 64-bit hash of the authoring state (see "Determinism") |
| `world.diff` | `{snapshot, fields?, limit = 50}` → `{changed: [{id, name?, fields: {path: [old, new]}}], created, destroyed, count, more?}` against a named snapshot |
| `input.inject` | `{events: [...]}` → `{queued, firstStep, lastStep}` (see "Input") |
| `input.state` | → `{actions: {name: {held, pressed, released}}, axes: {name: value}}` for the current step |
| `input.record` | `{path}` starts, `{stop: true}` stops → `{path, steps, events}` |
| `input.replay` | `{path, verify = true}` → `{steps, events, checkpoints, frame}`; `E_REPLAY` with the first diverging step |
| `log.read` | `{since?, level = info, channel?, limit = 50}` → `{lines: [{seq, t, level, channel, msg, repeat?}], next, dropped?}` |

## Live host

### One loop, two hosts

`runtime::LiveHost` owns the window, the renderer, the World and the session's host
object. It replaces the body of `Application::run` and serves both `ohao_game` (no
protocol) and `ohao serve --window` (protocol). The headless host is `RenderHost` grown
into a `Host` implementation: no window, renderer started on first capture as in spec 3.

Per frame, in the window host:

```
poll window events                      -> device events for the input stamper
drain the request queue                 -> run requests in order (lockstep: wait up to 16 ms for one)
advance the clock                       -> realtime: steps from wall time; lockstep: 0 here
for each step: input.beginStep(stamped events); world.simulate(fixedDt); recorder.step()
world.sync()                            -> instantiate, transform, render extract
render + present                        -> skipped while nothing changed in lockstep
complete pending captures               -> readback of the frame just rendered, encode off-thread
```

### System phases

`World` systems gain a phase:
- **simulate** systems (`spin`, `fly_controller`, later physics and gameplay) run once per
  fixed step with the fixed dt: `World::simulate(dt)`.
- **sync** systems (`instantiate`, `transform`, `render_extract`) run once before each
  render and after each batch of steps: `World::sync()`. They take no dt.
- `World::update(dt)` stays as `simulate(dt); sync();` for tests and the CLI.
- `addSystem` takes the phase; the existing call sites declare theirs. `world.step` runs
  `simulate` per frame and `sync` once at the end.

This makes the window's simulation independent of the frame rate: a realtime session and
a lockstep session that run the same steps with the same input reach the same state.

### Time modes

- **lockstep** (default for `ohao serve`, both hosts): simulation advances only through
  `world.step` and `input.replay`. Between requests the window keeps polling and
  re-presents its last image; rendering a new frame does not change world state.
- **realtime** (default for `ohao_game`): the clock steps from wall time as today, the
  human plays, and the agent can still query, edit, capture and read logs between frames.
  `time.set {paused: true}` stops stepping; `world.step` then works as in lockstep;
  `paused: false` resumes from the current state.
- Switching mode never loses or invents a step: the stamped-step counter (`frame`) is
  the single time axis for input, recordings, hashes and logs.

## Input

### Bindings are data

`InputMap` moves to `ohao/core/input` (unchanged behaviour), so ECS systems can read it.
The host keeps one `InputMap` per World and steps it per *simulation step*, not per frame.

A new authoring singleton holds the bindings, so a scene file carries its controls and an
agent can read and edit them like any component:

```cpp
struct ActionBinding { std::string action; std::string key; };               // "jump", "Space"
struct AxisBinding   { std::string axis; std::string positive; std::string negative; };
struct InputActions  { std::vector<ActionBinding> actions; std::vector<AxisBinding> axes; };  // singleton
```

- Key names come from one table in `ohao/core/input` (`"W"`, `"Space"`, `"LeftShift"`,
  `"Escape"`, `"MouseLeft"`, `"MouseRight"`, …), mapped to the GLFW codes `InputMap` already
  uses. An unknown key name is `E_ENUM` with a suggestion, in files and commands.
- The host rebuilds the `InputMap` bindings when `InputActions` is constructed or changed
  (an `on_construct`/`on_update` signal, as instantiate uses).

### Injection

`input.inject {events}` queues events stamped by simulation step, relative to the next
step to run (`step: 0`):

```json
{"events": [
  {"step": 0,  "axis": "move_forward", "value": 1},
  {"step": 60, "axis": "move_forward", "value": 0},
  {"step": 61, "action": "fire", "down": true},
  {"step": 62, "action": "fire", "down": false},
  {"step": 70, "key": "LeftShift", "down": true},
  {"step": 70, "mouse": [12, -4]}
]}
```

- **Through a virtual device.** Injected actions and axes act as one more device in
  `InputMap`, so `pressed`/`held`/`released` mean exactly what they mean for a keyboard. An
  injected value holds until changed (`value: null` releases an axis back to its keys).
  Injected keys go through the bindings like real keys. Mouse entries are deltas.
- **Order.** Events of the same step apply in list order, after that step's real device
  events. `step` must be ≥ 0; events cannot be stamped into the past.
- In realtime mode a human's device events are stamped with the step that consumes them,
  so a recorded session is exactly reproducible in lockstep.

### Recording and replay

`input.record {path}` writes a JSON-Lines file under the output directory:

```
{"format":"ohao.input","version":1,"schemaHash":"…","scene":"assets/scenes/helmet_game.scene.json","seed":"…","startFrame":0,"startHash":"…","fixedDt":0.016666668}
{"step":0,"axis":"move_forward","value":1}
{"step":60,"hash":"…"}
…
{"end":600,"hash":"…"}
```

- Every event that reached the `InputMap`, real or injected, with its absolute step.
- A checkpoint `{"step", "hash"}` every 60 steps, and the final hash.
- `input.replay {path}` requires the world's hash to equal `startHash` (else `E_REPLAY`:
  load the scene with the seed, or restore a snapshot, first), feeds the events in
  lockstep, compares every checkpoint, and fails with the first diverging step.
- The recording is plain data: an agent can read it, trim it or write one by hand.

### `FlyController`, the first playable system

An engine-provided component and simulate-phase system, so a scene is playable without
host code:

```cpp
struct FlyController { float moveSpeed{3.0f}; float fastMultiplier{4.0f}; float lookDegPerUnit{0.1f};
                       std::string moveForward{"move_forward"}, moveRight{"move_right"}, moveUp{"move_up"},
                                   look{"look"}, fast{"fast"}; };
```

It moves and turns its entity from the named axes and actions, exactly as `ohao_game`'s
hook does today. Put on the `Camera` entity, it gives the fly camera. `ohao_game` becomes a
scene file (`assets/scenes/helmet_game.scene.json`: the helmet room, the spinning cube as a
`Spin`, the camera with a `FlyController`, `InputActions`) played by `LiveHost`.

## Frame capture

`render.capture` depends on the host:

- **Headless host:** as spec 3 (fresh accumulation, N frames, PNG), plus `inline`.
- **Window host, lockstep:** captures the current world state deterministically: reset
  the temporal state (`resetAccumulation`), render `frames` frames of the unchanged world
  (default: 1 with no temporal effect on, 10 with TAA, spp + 3 in the path tracer), read
  back the last. The image equals a headless capture of the same world state at the same
  size, bit for bit in deferred (tested). `width`/`height` are the window's; asking for
  another size is `E_COMMAND`.
- **Window host, realtime:** the next frame the window presents, as is, with no reset
  (`deterministic: false`).

**Readback on request only.** A capture asks the renderer to copy its final LDR image into
a persistently mapped staging buffer at the end of the next frame and waits for *that
frame's* fence, never the queue. Frames without a pending capture copy nothing: the
presentation gate's unrequested-readback count stays 0, and the gate checks it.

**Output.**
- `path`: a PNG under the output directory (lossless; `stb_image_write` unless the capture
  bench shows the PNG encode over budget, in which case `fpng` replaces it in the capture
  path, pinned, as a ledgered ruling). Default.
- `inline: true`: a JPEG preview (quality 85) scaled so its long side is at most `maxSize`
  (default 768 px; at Claude's ~w·h/750 image cost a 768x432 preview is ≈ 440 tokens). Over
  MCP it is an `image` content block; over native JSON-RPC, base64 in `image`.
- `region: [x, y, w, h]` crops before scaling, so an agent can zoom in without paying for
  the whole frame.
- Result: `{path?, image?, width, height, frames, ms, deterministic, validation?}`.

Capturing while the window is minimized is `E_STATE`.

## Logging

`ohao/core/log`:

```cpp
enum class Level : uint8_t { Trace, Debug, Info, Warn, Error };
// Formats only when `level` passes the global threshold (one load and compare otherwise).
#define OHAO_LOG(level, channel, ...)
```

- **Channels:** `render`, `gpu`, `validation`, `ecs`, `asset`, `physics`, `audio`, `api`,
  `rpc`, `runtime`. Vulkan validation messages go to `validation`, so an agent sees them
  live, with the frame they happened in.
- **Ring buffer:** the last 4,096 records (`seq`, time since start, `frame`, level,
  channel, message cut at 1 KB). Identical consecutive records collapse into one with a
  `repeat` count. `log.read` pages it; `dropped` reports records that fell off.
- **Sinks:** stderr (human format; the default) and the RPC sink (notifications at the
  client's level). Both are optional; the ring is always on.
- **Migration:** every `std::cout`/`std::cerr`/`printf` under `ohao/` moves to `OHAO_LOG`
  with a channel and level. A source lint then forbids them under `ohao/` (tests and tools
  keep their own output).

*As built (step 4, 2026-10-23).* `ohao/core/log.{hpp,cpp}`. `OHAO_LOG(Info, render, "…{}",
x)`: level and channel are tokens, the rest a `std::format` string checked at compile time;
below the threshold the arguments are not evaluated. Above it the message is formatted into
a thread-local buffer, cut at 1 KB on a whole UTF-8 character, and written to the ring under
one mutex. A repeat raises the newest record's `repeat` in place (no new `seq`), so a reader
past it does not see the new count. Sinks have their own level, receive `write` and
`repeated` under the logger's lock in record order, and must not log. The stderr sink prints
`<L> <channel>: <msg>` (`L` in `TDIWE`) and folds a run of repeats into
`... repeated N more times`. `OHAO_LOG_LEVEL` sets the threshold at start-up. The renderer
stamps the frame (`log::setFrame`) as each deferred or RT frame starts. Validation messages
are `Error`/`Warn` records on `validation` as `<VUID> | <text>`; `check_validation.py` reads
those lines and the repeat lines. The old `ConsoleWidget` facade (five physics files, its
own `OHAO_LOG(msg)` macro) is deleted. The print lint is `reflect_tests` `sp4_t02_*` (whole
words: `snprintf` is not `printf`; the logger's own stderr sink is the one allowlisted use).
The state is a leaked singleton, so records written during static teardown are safe.

## Determinism

Spec 3's contract, extended:

1. **World state.** A lockstep session (headless or window) gives bit-identical world
   state and identical replies across runs of the same build, given the same scene, seed,
   method sequence and injected input. `world.hash` exposes it: FNV-1a 64 over the
   snapshot columns (spec 3's capture, so the cost is a snapshot plus a hash pass), the ID
   generator state and the creation sequence, in that order.
2. **Realtime sessions are not reproducible as such.** Their recordings are: stamped by
   step, they replay in lockstep to the same checkpoints.
3. **Deferred pixels.** After step 1, deferred rendering, with or without TAA, SSAO and
   bloom, is bit-identical run to run on the same machine and driver, and a lockstep live
   capture equals a headless capture of the same state. Cross-machine equality is not
   promised.
4. **RT pixels** keep their golden tolerances: RT offline's rare 1-LSB flips and
   `rt_realtime`'s noise are 2b's to fix.
5. **Sources of nondeterminism are linted.** `std::random_device`, `rand(`,
   `std::chrono::*_clock::now` and `glfwGetTime` may not appear in simulate-phase systems
   or under `ohao/render`, `ohao/gpu` except on an allowlist (timing statistics, GPU timer
   queries, the default ID seed in `World`). A test enforces it.

### Should 4 fix the 1-LSB post-effect noise? Yes, in step 1.

Live capture equality, replay verification with pixels, and every agent A/B ("did my edit
change the image?") need deferred captures that do not move on their own. The cause found
in the code (the SSAO noise seed) is a one-line class of fix: seed it from the renderer's
seed (`setRenderSeed`). Step 1 then proves determinism pass by pass:
- two fresh renderers, Cornell and helmet, post effects off and on: bit-identical;
- `restore_render_gpu_test`'s post-effects case drops its 1-LSB allowance to exact;
- the helmet deferred goldens are regenerated (the noise pattern changes; the images are
  looked at first), ten `--selftest` runs are bit-identical, and their tolerances are
  tightened to exact. Tightening is not widening; the manifest says why.

If a residual difference remains after the seed fix, step 1 bisects it pass by pass with
the same two-renderer test and fixes what it finds. A residual that is traced to the
driver (not OHAO code) is measured, recorded in the change record and the manifest with
its measured floor, and goes to Frank as a ruling; it is never absorbed silently.

*As built (step 1, 2026-10-23).* The seed fix alone changed nothing visible: SSAO's
output is never sampled by the lighting shader (see "Why"). The bisection found the two
real causes, both in OHAO code:
- **Bloom raced its own mip chain.** Post-processing runs outside the render graph, and
  each bloom render pass's only external dependency guarded earlier *reads* against its
  writes. Nothing made the previous pass's color writes visible to the next pass's
  sampling. Bloom, TAA and the tonemap pass now take
  `RenderPassBase::sampledColorPassDependencies()`: earlier color-attachment and shader
  writes visible to this pass's sampling and load, and this pass's writes visible to
  later sampling and copies.
- **TAA's first frame read an unset matrix.** `DeferredRenderer::m_prevViewProj` was a
  default-constructed `glm::mat4` (uninitialized). TAA blends towards the cleared history
  by the motion it sees (`mix(blendFactor, 0.5, motionWeight)`), so a fresh renderer's
  first frame depended on stale memory, and a capture after `resetTemporalState` (which
  set the matrix) differed from a fresh renderer's. The first frame after creation or a
  reset now uses its own view-projection: no motion into it.
- **The SSAO seed** is still fixed as specified (noise from the render seed, pinned for
  seed 0, `setRenderSeed` reaches the pass), and RT renderers created after
  `setRenderSeed` now receive the seed too (they did not).

Result: every deferred configuration (Cornell plain, each post effect alone, all three;
the helmet plain and with post) is bit-identical across fresh renderers, across
processes and across repeated captures by one renderer; ten separate-process renders of
each deferred golden gave one pixel hash; the four deferred goldens are exact. Wiring
SSAO into lighting is a look change and goes to 2b's renderer findings.

## Security

- **No listener by default.** stdio serves exactly the parent process. There is no port,
  no TCP, no WebSocket in 4.
- **Named pipe (opt-in, `--pipe NAME`):** created with `PIPE_REJECT_REMOTE_CLIENTS`, a
  DACL granting only the current user, and one instance (a second client is refused).
- **File sandbox.** Paths in params (`scene.load`, `scene.save`, `render.capture`,
  `input.record`, `input.replay`) are project-relative with `/`, resolved by the spec-3
  resolver rules (no absolute paths, no `..`). Reads are confined to `--root` (default:
  the working directory); writes to `--out` (default `<root>/agent_out/`, git-ignored).
  Anything else is `E_ACCESS`. `--allow-path DIR` (repeatable) adds a root explicitly.
- **No code execution.** No method runs a shell, loads a library or evaluates script in 4.
- **Bounded work.** Message size, batch size (10,000 commands), `world.step` frames
  (1,000,000 per call) and log page sizes are capped; long work is cancellable.
- **Data is data.** Names and strings in scene files reach the agent verbatim. The agent
  guide says so: a scene from elsewhere can contain text written to steer an agent, and the
  server never interprets it.

## Token budget (agents)

Spec 3's rules (defaults omitted, projection, summaries, capped errors) carry over.
Additions in 4:
1. **`tools/list` ≤ 24 KB**, gated. One-line descriptions; schemas without prose beyond
   field docs.
2. **`world.step` with `watch`** returns only the watched fields: a play loop of
   inject + step + read is one or two calls.
3. **`world.diff`** answers "what changed?" with only the changed fields of only the
   changed entities, capped at `limit` with a `more` count.
4. **Image previews** default to ≤ 768 px on the long side (≈ 440 tokens), JPEG; `region`
   zooms; full-resolution PNGs go to files the agent opens only if it must.
5. **Logs** are pulled by level and channel, 50 lines a page, each line ≤ 1 KB, repeats
   collapsed; notifications only at the level the client asked for.
6. **Golden and check results** are one JSON line plus files: the agent reads the summary
   and opens a heatmap only for a failure.

Estimates (to be replaced by measurements in the change records, as in spec 3):

| Operation | ≈ tokens |
|---|---|
| Connecting: `initialize` + `tools/list` | ≤ 6k, once |
| `world_step` 60 frames, watching one field of one entity | 60 |
| `input_inject` of a held axis for 60 steps | 50 |
| `render_capture` inline 768x432 | 440 image + 40 text |
| `log_read` 10 lines | 300 |
| `world_diff` after a play of 600 steps, 5 entities moved | 250 |

## Performance

Performance stays a gate. New hot paths and the protocol get budgets in
`tests/bench/serve_budgets.json`, measured by `serve_bench` (Release) on the reference
machine (Frank's dev laptop).

### Measurement rules (lessons from 3)

Spec 3's restore-scaling saga showed a gate can fail on its measurement rather than on the
code. Every budget in 4 is measured under these rules from the start:
- **Like for like.** Two numbers compared (a ratio, or a transport overhead against an
  in-process call) are measured in the same cache and memory regime, in blocks, with page
  faults reported next to them.
- **Absolute budgets first.** A ratio gate is used only where its denominator is large
  compared with its noise; spec 3's 2 ms denominator moved ±40% with heap history.
- **Calibration.** Each run times a fixed CPU loop before and after the gated blocks. If
  either is more than 25% slower than the machine's recorded reference
  (`build/serve_bench_reference.json`, written by `--calibrate`), the run is reported as
  `noisy` with exit code 2, distinct from a regression (exit 1). The check script reruns a
  noisy bench at most twice and reports every attempt. One `scene_bench` run on 2026-10-23
  (during the Godot cleanup) was 2.5× slow on every operation from outside load, and the
  next passed; this rule exists for that.
- **Medians of 50 after 5 warm-ups, p95 reported**, high process priority, as in spec 3.
- **A budget is accepted when five consecutive gate runs pass** at the step that adds it.
- **Budgets are provisional until their step measures them.** A budget that the built
  code misses is reported to Frank with the measurements and the options; it is not
  changed by the step (spec 3's open-decision-1 process).

### Budgets

CPU (`serve_bench`, no GPU; the server is a child process on pipes, as an MCP client runs
it):

| Operation | Budget (median) |
|---|---|
| `world.info` round trip over stdio, bench10k loaded | ≤ 0.15 ms |
| Transport overhead: that round trip minus the in-process `Session::call` | ≤ 0.10 ms |
| `world.query` one entity, one projected field, over stdio | ≤ 0.20 ms |
| `world.apply` 100 `field.set` over stdio | ≤ 0.40 ms |
| `world.step` 1 frame, bench10k with 100 spinning entities, headless | ≤ 0.50 ms |
| `world.hash` bench10k | ≤ 1.0 ms |
| `world.diff` bench10k against a snapshot, 1% of rows changed | ≤ 2.0 ms |
| `OHAO_LOG` below the threshold | ≤ 5 ns per call |
| `OHAO_LOG` into the ring, 80-character message | ≤ 300 ns per call |
| `tools/list` size | ≤ 24 KB |

GPU (window host, Cornell deferred, 1280x720, lockstep, no post):

| Operation | Budget (median) |
|---|---|
| `world.step` 1 frame → reply after the frame is presented | ≤ 8 ms |
| Readback of one 1280x720 frame (copy + that frame's fence) | ≤ 2 ms |
| `render.capture` inline (768 px JPEG) end to end | ≤ 20 ms |
| `render.capture` PNG file 1280x720 end to end | ≤ 30 ms |
| Reader-thread CPU time over a 600-frame idle realtime session | ≤ 1 ms total |
| `ohao_game` presentation gate | unchanged: 0 unrequested readbacks, 0 validation messages incl. teardown |

Recorded, not gated: process start to `initialize` reply (headless and window), Cornell
load-to-first-frame in the window host, `tools/list` and `instructions` byte counts.

## Engine development by agents (G2)

### One entry point: `python tools/check.py`

```
python tools/check.py [--labels quick,gpu,docs] [--all] [--jobs 6] [--config Release] [--no-build]
```

1. **Preflight:** Python dependencies the harnesses need (numpy, Pillow, jsonschema),
   and the CMake to use: `OHAO_CMAKE`, else the scoop shim, else `PATH`; a conda `cmake`
   is refused with the fix in the message (gotcha 1, closed).
2. **Configure** if `build/CMakeCache.txt` is missing.
3. **Build the `shaders` target first, then everything,** with `--parallel 6` by default
   (the machine's limit; `--jobs` lowers it). Stale SPIR-V can no longer pass (gotcha 2,
   closed).
4. **`ohao doctor`** (below); an error stops the run with its verdict.
5. **`ctest`** with the chosen labels, `--output-junit build/check/junit.xml`,
   `--output-on-failure`, `--no-tests=error`. GPU tests share a `RESOURCE_LOCK` named
   `gpu`, so they never overlap; CPU tests run 4 at a time.
6. **Summary:** `build/check/check.json`:
   `{ok, labels, build: {ok, seconds}, doctor, tests: [{name, labels, ok, seconds, passed?,
   failed?, failures?, tail?}], counts}`. `passed`/`failed`/`failures` come from each
   suite's JSON line; `tail` is the last 40 lines of a failing test's output, so a failure
   is never lost (one `restore_render_gpu_test` failure in 3 was). The same object is the
   last line of stdout. Exit 0 or 1.

Default labels: `quick` (CPU suites), `gpu`, `docs`. `--all` adds `golden`, `bench` and
`slow`. A step-chain runs `python tools/check.py --all` as its gate.

*Revision of spec 3* ("a `check` target, native CMake, no new runtime"): CMake's script
mode cannot read JUnit or the suites' JSON lines without re-implementing a parser in CMake
language, and the harnesses already need Python. `check.py` is a test harness (stdlib
only), not shipped and never on an engine path, which the language table allows. CTest
remains the registry: `ctest -L quick` works without the script.

### CTest registration

- `enable_testing()` at the top level; every suite, harness and gate is an `add_test`
  with labels: the C++ suites, the `*_gpu_test`s, `check_cli.py`, `check_scene_schema.py`,
  `check_validation.py`, the `ohao_game` presentation gate, `render_golden.py`,
  `scene_bench --check`, `serve_bench --check`, `check_docs.py`, the monograph checks.
- A test compares the registered tests with the test executables the build produces, so a
  new suite cannot be forgotten.
- `tests/CMakeLists.txt` (dead gtest) is deleted.
- Source globs gain `CONFIGURE_DEPENDS`, so a new `.cpp` is picked up by the next build
  without a manual re-configure (gotcha 3, closed); the no-op build time is recorded
  before and after in the step's change record.
- Every executable that loads SPIR-V depends on `shaders`, the examples included.

### Machine-readable output everywhere

- `tests/common/harness.hpp`: the `CHECK`/`SECTION` macros and the final
  `{"suite", "passed", "failed", "failures"}` line, adopted by every C++ suite that lacks
  it (`engine_tests`, `ecs_tests`, `runtime_tests`, physics, audio, force, `env_cdf_test`,
  `denoise_parse_test`, `sobol_test`, `renderer_test`, the `*_gpu_test`s).
  - *As built (step 2):* the macros are `CHECK`/`RUN` (the names the suites already used;
    there is no `SECTION`), and `ohao::test::finish(...)` prints the line, either from the
    macros' counters or from a suite's own (the `TEST_BEGIN`/`EXPECT` suites keep their
    macros and record failures). The three GoogleTest suites link `ohao_gtest_main`, a
    main that prints the line. The `*_gpu_test`s already printed it.
- `render_golden.py --json FILE` and `check_validation.py --json FILE` write one object
  per scene (below). `check.py` passes them.

### Golden diffs an agent can read

For each failing scene (or every scene with `--heatmaps all`), `render_golden.py` writes:
- `build/golden_diff/<name>.heat.png`: per-pixel max channel difference, amplified and
  color-mapped, at the compare resolution;
- `build/golden_diff/<name>.crop.png`: expected | actual | heat of the worst region at
  full resolution, side by side;
- in the JSON: `{name, pass, max_abs, diff_frac, diff_px, rmse, tolerance, regions:
  [{x, y, w, h, max_abs, px}]}` with at most 4 regions (connected components on a 16x16
  grid, largest first).

`--selftest --json` reports the measured run-to-run floor per scene, so "is this my change
or noise?" has a number.

*As built (step 3), with two corrections:*
- **The crop is at the compare resolution, not full resolution.** The goldens are stored
  downscaled to 640 px wide, so there is no full-resolution expected image to crop. The
  strip is expected | actual | heat at 640-px scale, 4x nearest-neighbour.
- **The crop is bounded.** Scattered noise (the realtime path tracer's) merges into one
  region as large as the image; a 4x crop of it would be several thousand pixels wide,
  too costly to look at. A region larger than 96x64 is cropped to a 96x64 window centred
  on its largest difference, so a strip is at most 1168x256.
- Regions are connected components (4-neighbour) of 16x16-pixel cells; the JSON lists the
  4 largest and a `more_regions` count. The heatmap is per-pixel max channel difference,
  black where equal, then dark red to white (any difference at least 191 red, the largest
  white). `render_golden.py` and `check_validation.py` print the shared summary line, and
  CTest runs them with `--json` into `build/golden_diff/golden.json` and
  `build/check_validation.json`.

### `ohao doctor`

`ohao doctor [--cpu-only]` prints `{ok, checks: [{id, ok, level, detail, fix?}]}` and
exits 0 (warnings allowed) or 1. Checks:
- `vulkan.instance`, `vulkan.device` (name, driver, API version), `vulkan.rt` (ray tracing
  pipeline and acceleration structure support), `vulkan.validation` (layer available);
- `shaders.complete` (every source has its SPIR-V) and `shaders.fresh` (no SPIR-V older
  than its source or any include it reads, listing the stale ones);
- `assets.present` (every asset the checked-in scenes reference);
- `tools.glslc`, `tools.cmake` (warns when `PATH`'s cmake is conda's), `python.deps`;
- `gpu.wedged`: a 64x64 deferred frame within 2 s; after a crash the GPU has stayed wedged
  until reboot, and this says so instead of letting 30 s renders look like regressions.

*As built (step 3):* the verdict is printed inside the CLI's envelope, `{"result": {ok,
cpuOnly, checks}}`, like every other `ohao` command (the spec showed the bare object).
Each check names its failure `level`: `error` (exit 1: the Vulkan instance and device,
shaders, assets, glslc, the GPU probe) or `warn` (ray tracing, the validation layer,
conda's cmake, the Python modules). `--shader-dir` and `--spirv-dir` point the shader
checks at another tree (the test uses fake ones); the SPIR-V checked is the copy the
engine loads, `<exe dir>/bin/shaders`. `#include` resolution follows glslc: a quoted
include in the including file's directory first, then `shaders/` and `shaders/includes/`;
an include that resolves nowhere is named in the detail. `tools/check.py` runs the doctor
with `--cpu-only` unless a `gpu` or `golden` label is selected, and stops (no tests) when
it fails; `--no-doctor` skips it.

### Recipes added to `docs/ARCHITECTURE.md`

Each written and then followed once, as in 3: add a render pass; add a shader (and its
SPIR-V name); add a golden scene; add an API method (now: params struct, method, catalogue,
MCP tool, all from one place). Plus the agent guide `docs/agents/ohao-serve.md` (connect,
orient, edit, play, capture, replay, read logs), checked by `check_docs.py`.

### Lessons from sub-project 3, and the rule each became

1. **Permissions.** Sessions here could not write under `.claude/`, and rm, shell loops,
   redirections and compound commands were denied. → Every gate is one argv command that
   writes its own outputs under `build/`. No step writes under `.claude/`. What only Frank
   can do is collected under "Frank actions", not discovered mid-step.
2. **A flaky ratio.** `restore_empty`'s 12× gate flipped on the noise of a 2 ms
   denominator and heap history. → "Measurement rules" above.
3. **1-LSB post-effect noise.** Tolerated in 3, traced in 4 to an unseeded SSAO kernel.
   → Fixed in step 1, because live capture and replay depend on it.
4. **A failure without its output.** → `check.json` keeps each failure's tail.
5. **Machine stability.** Builds above `-j6` have crashed this machine. → `check.py`
   defaults to 6 jobs; GPU tests are serialized.
6. **Transient load.** One gate run was 2.5× slow on everything. → Calibration and the
   `noisy` exit code.
7. **Docs drift.** → `check_docs.py` covers the new guide and recipes; `check.py` runs it.
8. **Git Bash rewrites `/paths`.** → Protocol params never go through a shell; the CLI's
   `Type/field` form stays.

## Programs

- **`ohao serve [scene] [--window [WxH]] [--lockstep | --realtime] [--root DIR] [--out DIR]
  [--allow-path DIR] [--seed S] [--pipe NAME] [--log-level L] [--vsync]`**: headless by
  default; `--window` is the live host. Lockstep by default.
- **`ohao doctor [--cpu-only]`** (above).
- **`ohao run`** gains nothing new but runs on the same host interface; its goldens stay.
- **`ohao_game [scene]`**: the live host in realtime with no protocol; default scene
  `assets/scenes/helmet_game.scene.json`. Its flags and the presentation gate are
  unchanged.
- **`serve_bench [--check FILE] [--cpu-only] [--calibrate]`**: the new budgets.
- **`.mcp.json`** at the repo root: the project MCP server
  (`build/Release/ohao.exe serve --window assets/scenes/helmet_game.scene.json`), so Claude
  Code offers it in this repo after Frank approves it once.

## Test plan

CPU (no device):

1. **Deferred determinism** (GPU, listed here for its number): two fresh renderers, Cornell
   and helmet, post effects off and on, bit-identical; `restore_render_gpu_test` post case
   exact; ten golden `--selftest` runs identical for the deferred scenes.
   - *As built (step 1):* `deferred_determinism_gpu_test`. Seven configurations (Cornell
     plain, TAA, SSAO, bloom, all three; helmet plain and with post), each rendered by two
     fresh renderers, by a fresh renderer in a child process of the test, and twice by one
     renderer: all bit-identical, validation clean. The SSAO noise is checked directly
     (deterministic, pinned hash for seed 0, unit vectors, another seed differs) and the
     seed's path to the pass, since no pixel shows SSAO. The ten-run check used full-
     resolution pixel hashes of each deferred golden command in separate processes.
2. **Source lints:** the nondeterminism lint (see "Determinism"); no
   `std::cout`/`std::cerr`/`printf` under `ohao/` (after step 4); `ohao_rpc` includes no
   Vulkan, GLFW or renderer header.
3. **Check entry point:** a fixture test labelled `selftest` (excluded by default) that
   fails makes `check.py --labels selftest` exit 1 with the failure and its tail in
   `check.json`; `check.json` and `junit.xml` parse; every built test executable is
   registered.
   - *As built (step 2):* `tests/check/check_tests.py` (label `quick`). The fixtures are a
     Python-only CMake project (`tests/check/fixture`) configured into its own build tree,
     `build/check_fixture`: a CTest run nested inside the main one would share
     `build/Testing`. Seven checks: the failure fails the check (exit 1, counts); it
     carries its suite line and a 40-line tail; stdout's last line is `check.json` and the
     failing output stays out of the progress lines; `junit.xml` agrees; labels select
     exactly their tests; an unknown label exits 2; every `*_test`, `*_tests` and
     `*_bench` executable in `build/Release` is a registered test, and every test has
     exactly one known label.
4. **Golden harness:** synthetic image pairs with known differences give the expected
   max_abs, regions and heatmap pixels; `--json` shape.
   - *As built (step 3):* `tests/golden/test_golden_diff.py` (quick, 11 checks), including
     the bounded crop of a large region and an end-to-end run of the harness on a fake
     manifest whose command copies a prepared PNG (failing scene: regions, heatmap, crop,
     summary line; passing scene: no images unless `--heatmaps all`; selftest floor).
5. **Doctor:** a fake shader tree (temp directory) with one stale and one missing SPIR-V
   is reported exactly; JSON shape; `--cpu-only` skips the device checks.
   - *As built (step 3):* `tests/doctor/check_doctor.py` (quick, 7 checks: missing,
     stale through a quoted include, stale through two includes via the include root,
     a newer source, an unresolved include, the shape and `--cpu-only`, the real tree)
     and `--gpu` (gpu, the device checks pass on this machine).
6. **Logger:** level filtering, ring wrap and `dropped`, repeat collapse, channel filter,
   `since` paging, concurrent writers (no torn records), sink routing.
   - *As built (step 4):* `log_tests` (quick, 10 tests `sp4_t06_*`): also arguments below
     the threshold are not evaluated, the frame and time stamps, the 1 KB cut on a UTF-8
     boundary, level and channel names, and the stderr line format with repeats folded
     (stderr redirected to a file).
7. **JSON-RPC framing:** CRLF, partial reads across buffer boundaries, oversize lines,
   invalid JSON (-32700), non-objects and batches (-32600), ids echoed byte for byte
   (strings, negatives, 2^63), notifications get no reply, unknown methods (-32601 with a
   suggestion), the error-code table, cancellation mid-step leaves a whole step.
8. **MCP:** version negotiation (both supported versions and an unknown one);
   `tools/list` covers the method table with mapped names and is ≤ 24 KB; every
   `inputSchema` is valid draft 2020-12 and accepts each method's examples from the agent
   guide (through `jsonschema`); `tools/call` success and `isError`; `logging/setLevel` and
   `notifications/message`; `ping` answered while a 10,000-step request runs.
9. **Protocol goldens:** scripted NDJSON sessions against `ohao serve` (headless, CPU)
   with checked-in expected stdout, byte-identical twice (the `check_cli.py` pattern).
10. **Stdout stays clean:** a test-only build of the server with a method that prints to
    stdout and stderr through every C and C++ path; the stream still parses.
11. **Time model:** simulate systems run once per step, sync once per batch; lockstep
    counts are exact; `world.step watch`; `E_STATE` rules per host and mode; `dt` default;
    a realtime-paused-stepped sequence reaches the same hash as pure lockstep.
12. **Hash and diff:** the hash is stable across save → load with the same seed, changes
    on any authoring change and ignores runtime components; `world.diff` equals a diff of
    the two saves, respects `limit`/`more`.
13. **Input:** key-name table round trip; `InputActions` rebinds on change; injection gives
    `pressed` on exactly the stamped step, `held` after, `released` on the release step;
    axis override and release; `FocusLost`; record → replay matches every checkpoint; a
    tampered recording fails with the right step; a realtime recording with dropped time
    (the `maxSteps` clamp) replays exactly.
14. **`FlyController`:** 60 steps of `move_forward = 1` move the entity by exactly
    `moveSpeed · dt · 60` along its front; look turns by the configured rate; `fast`
    multiplies.
15. **Security:** absolute paths, `..`, and writes outside `--out` are `E_ACCESS`;
    `--allow-path` admits exactly its root; a second pipe client is refused; the size
    and step caps hold.

GPU:

16. **Live host:** a test client drives `ohao serve --window` (hidden window allowed):
    load, apply, inject, step, capture, read logs; lockstep capture equals a headless
    capture of the same state bit for bit (deferred, post off and on); 0 validation
    messages through teardown; 0 unrequested readbacks; a disconnect mid-step exits 0 with
    clean validation; capture while minimized is `E_STATE`.
17. **`ohao_game` from a scene:** the presentation gate passes unchanged; the scene file
    saves byte-identically after load; the fly camera responds to injected input as test
    14 says.
18. **Benchmarks:** `serve_bench --check` passes every budget, five consecutive runs.
19. **End-to-end MCP acceptance:** `tests/serve/e2e_mcp.py` (stdlib-only MCP client)
    runs: initialize → load `helmet_game` → summary → apply an edit → record → inject a
    60-step flight → step with `watch` → capture inline and to a file → log read →
    stop recording → restore → replay (hashes match) → capture again (bit-identical to the
    first). The manual run with Claude Code is recorded in the step 11 change record.
20. **Docs:** `check_docs.py` runs every command in `docs/agents/ohao-serve.md` and the new
    recipes.

All existing suites, goldens, validation runs and `scene_bench` stay green throughout.

## Done criteria

All verified by running, none estimated.

- Tests 1–20 pass; every existing suite, golden and gate stays green.
- `python tools/check.py --all` exits 0 and its `check.json` lists every test.
- The end-to-end MCP run passes, and the manual Claude Code run is recorded.
- `serve_bench --check` passes every budget, five consecutive runs; results in the change
  records.
- `docs/agents/ohao-serve.md`, the new recipes and the `AGENTS.md` updates exist, with
  every command run.
- The monograph gains an "Agent control surface" module with citations resolving.
- The roadmap row is marked done; each step has a change record (written by a Haiku agent
  and reviewed) in `docs/changes/`.

## Build order

Each step ends with: the engine running, the step's tests and every existing suite green
(from step 2 on, `python tools/check.py --all`), a change record
`docs/changes/YYYY-MM-DD-agent-surface-stepN.md`, and one or a few local commits. A step
that hits a budget it cannot meet, a golden change it cannot explain, or a need for
`.claude/`, the network or a new dependency not named here, stops and reports instead of
improvising. Steps are ordered so the G2 tooling exists before the protocol work it gates.

1. **Deterministic deferred rendering.** Seed the SSAO noise from the renderer seed; the
   nondeterminism lint; the two-renderer test; regenerate and tighten the deferred
   goldens after looking at them; `restore_render_gpu_test` exact. Tests 1, 2 (lint part).
   - *Done 2026-10-23.* Also fixed: the bloom/TAA/tonemap render-pass dependencies and
     TAA's unset first-frame matrix (the actual causes; see "Determinism", as built), and
     the render seed not reaching RT renderers created later. Only the helmet deferred
     golden was regenerated (the Cornell one already matched); all four deferred
     entries are exact. The lint is `reflect_tests` `sp4_t02_*`. The `verify` skill change
     waits in `docs/agents/skills/` for Frank.
2. **One check entry point.** `enable_testing()`, every suite and harness registered with
   labels and the `gpu` lock, the dead `tests/CMakeLists.txt` deleted, `CONFIGURE_DEPENDS`,
   shaders as a dependency of every SPIR-V consumer, `tests/common/harness.hpp` adopted by
   every suite, `tools/check.py`, `AGENTS.md` and the `verify` skill text updated (the
   skill file change is a Frank action). Test 3.
   - *Done 2026-10-23.* 29 tests registered (`quick` 14, `gpu` 10, `docs` 2, `golden`,
     `bench`, `slow` 1 each); `python tools/check.py` 26/26 and `--all` 28/29 with
     `scene_bench` the one failure, a uniform 2.3× slowdown of unchanged code (see the
     step's change record). Also built: a `LOCKS` registration option, after the first
     run found three tests racing on `build/cli_tmp` (the CLI check runs as two tests and
     inside the doc checker) and `api_tests` writing a fixed `%TEMP%` file;
     `renderer_test` checks what it renders and writes under the build directory (it left
     `smoke_test.png` in the repo root). `doctor` is `null` in `check.json` until step 3.
     The `build` and `verify` skill drafts wait in `docs/agents/skills/`. The dead
     `tests/CMakeLists.txt` and the `tests/engine_tests.cpp` it named are deleted;
     `renderer_pipeline_tests.cpp` and `tests/shadow_system` (dead, but cited by the
     monograph) are left for a cleanup with their units.
3. **Golden diffs and doctor.** `render_golden.py`/`check_validation.py` `--json`,
   heatmaps, regions, selftest floor; `ohao doctor`; `check.py` runs doctor. Tests 4, 5.
   - *Done 2026-10-23.* See the as-built notes under "Golden diffs an agent can read" and
     "`ohao doctor`" (the crop is at the compare resolution and bounded; the doctor's
     verdict is in the CLI envelope). Also fixed: `check.py` read the wrong CMake cache key
     for the harnesses' Python interpreter (`Python3_EXECUTABLE`; FindPython keeps it as
     `_Python3_EXECUTABLE`), so its module check used whichever Python ran it.
4. **Logger.** `ohao/core/log`; every engine print migrated; validation messages into
   their channel; the print lint. Test 6, test 2 (print part); log budgets measured.
   - *Done 2026-10-23.* See "Logging", as built. The log budgets are measured by
     `log_bench` (`tools/bench`, label `bench`) against `tests/bench/serve_budgets.json`,
     not yet by `serve_bench`, which step 6 builds; step 6 folds them in. `log_bench`
     already applies the calibration rule (`--calibrate` writes
     `build/serve_bench_reference.json`; >25% slower is `noisy`, exit 2) and, at the
     reviewer's request, records every other process that used CPU during the run
     (`tools/bench/bench_env.hpp`): more than 50% of a core in total is also `noisy`.
     The check script's rerun of a noisy bench is still step 6's.
5. **Protocol core and headless `ohao serve`.** `ohao_rpc` (framing, ordering, errors,
   cancellation, progress), the MCP adapter, method params as reflected structs (the
   catalogue's method table gains schemas; `schemaHash` changes once), `engine.info`,
   `scene.load replace`, the file sandbox, `ohao serve` headless over stdio, protocol
   goldens. Tests 7–10, 15 (paths part).
6. **`serve_bench`, CPU budgets.** The child-process client, calibration, the CPU budget
   table, `tools/list` size gate. Test 18 (CPU part).
7. **Time model, hash, diff.** System phases, `World::simulate`/`sync`, lockstep and
   realtime clocks in `Application`, `time.get`/`time.set`, `world.step watch`,
   `world.hash`, `world.diff`. Tests 11, 12.
8. **Input as data.** `InputMap` to `ohao/core`, key names, `InputActions`, the virtual
   device, `input.inject`/`state`/`record`/`replay`, `FlyController`. Tests 13, 14.
9. **Live host and capture.** `LiveHost`, `ohao serve --window`, the request queue on the
   engine thread, lockstep render-on-demand, readback on request, inline previews,
   `ohao_game` on `helmet_game.scene.json`, GPU budgets in `serve_bench`. Tests 16, 17,
   18 (GPU part).
10. **Named pipe and limits.** `--pipe`, the DACL and remote rejection, single client, the
    remaining caps. Test 15 (rest).
11. **Acceptance and docs.** `tests/serve/e2e_mcp.py`, `.mcp.json`, the manual Claude Code
    run, `docs/agents/ohao-serve.md`, the recipes, `AGENTS.md`, the monograph module, the
    roadmap row, the final change record. Tests 19, 20; the full done criteria.

## Risks

- **MCP moves.** The protocol has had four dated versions since late 2024. Only `initialize`,
  `tools/*`, `ping`, logging and cancellation are used, which every version since
  2025-03-26 shares; supporting a new version is a table entry plus a test case.
- **Deferred determinism may have a second cause.** If the seed fix leaves a residue,
  step 1 bisects it; a driver-level residue becomes a measured, documented floor and a
  ruling for Frank, not a widened tolerance. *Step 1:* it had two (bloom sync, TAA's
  first frame), both in OHAO code and fixed; no driver residue.
- **The window host's loop changes under `ohao_game`.** Phases and stamped input change
  when the spin advances (per step, not per frame). The presentation gate and validation
  run catch breakage; the visual result is the same motion at a fixed rate.
- **`stb_image_write` PNG speed** may miss the capture budget; the fallback (fpng, pinned)
  is named above so the step does not stall.
- **Hidden windows on Windows** may not present the same way as visible ones. Test 16
  uses a visible window if a hidden one does not present; the step records which.
- **Third-party output on stdout** is caught by the descriptor redirect; test 10 proves
  it for C and C++ paths, but a library writing to the console handle directly would
  escape. None is known; test 10 would expose one.
- **Token cost of `tools/list` grows with every method.** The 24 KB gate makes that a
  visible decision rather than a drift.
- **Scope.** Eleven steps. The editor, network transports, gameplay code, physics in the
  ECS and the C ABI stay out.

## Review focus

Inputs no step's tests exercise by default, most likely to bite first:

1. A library printing to stdout in the middle of a capture: the stream must stay valid,
   and the next message must parse.
2. A cancel or a disconnect arriving between a capture's readback and its encode: no
   half-written file, no leaked staging buffer, validation clean at teardown.
3. Input injected while a human also holds keys in realtime, then paused and stepped: the
   recording must replay to the same hashes.
4. A window resize or minimize between `world.step` and `render.capture` in lockstep: the
   capture must fail cleanly or match the new size, never return a stale frame.
5. An agent asking for `world.query` of 100k entities with full components over MCP:
   capped by `limit`, under the message limit, with `next` to page.

## Decisions record

Frank delegated these choices (2026-10-23); they are decided here:

| # | Decision | Choice |
|---|---|---|
| 1 | Transport | stdio, newline-delimited JSON-RPC 2.0 (MCP's stdio transport). Corrects spec 3's `Content-Length`. |
| 2 | Native and MCP | One server answers both: native method names and MCP's `initialize`/`tools/*`. |
| 3 | Tool names | Method name with `.` → `_` |
| 4 | Tool results | One compact text block; no `structuredContent`/`outputSchema` (no duplicated tokens); images as image blocks |
| 5 | Params schemas | Method params are reflected structs: validation, catalogue and MCP schemas from one definition |
| 6 | Ordering | One engine thread, strict arrival order; `ping` and cancel on the reader thread |
| 7 | Error codes | -32700/-32600/-32601/-32602 for protocol; 1/2/3 (the CLI exit classes) for domain errors, `E_*` in `data` |
| 8 | Batches | None (MCP dropped them; `world.apply` batches atomically) |
| 9 | Time | Simulate and sync phases; lockstep default for `serve`, realtime for `ohao_game`; one step counter |
| 10 | Input | Bindings as an authoring singleton with key names; injection through a virtual device; stamped by step |
| 11 | Replay | JSON-Lines recordings with hash checkpoints every 60 steps |
| 12 | Capture | Lockstep: reset temporal state and render, equal to headless; realtime: presented frame; readback on request only |
| 13 | Images to agents | JPEG previews ≤ 768 px (inline), PNG files (lossless) |
| 14 | Logging | Own logger: levels, channels, 4,096-record ring, stderr and RPC sinks; all engine prints migrated |
| 15 | Deferred determinism | Fixed in 4 (step 1): seeded SSAO noise, pass-by-pass proof, deferred goldens tightened to exact |
| 16 | Security | stdio only by default; opt-in local named pipe for the current user; no network listener in 4; path sandbox; no code execution |
| 17 | Check entry point | `python tools/check.py` (test-only Python) over CTest labels; JUnit + `check.json`. Revises spec 3's "native CMake target". |
| 18 | Build safety | Shaders first, `-j6` default, GPU tests serialized, `CONFIGURE_DEPENDS` |
| 19 | Benchmarks | `serve_bench` with spec-3 methodology plus calibration and a `noisy` verdict; budgets accepted on five consecutive passes |
| 20 | Languages | C++20 for the server, host, logger and bench; Python only in test harnesses (stdlib for `check.py` and the e2e client). No new runtime. |
| 21 | New dependencies | None required. `fpng` only if the capture bench shows `stb_image_write` over budget (ledgered). |
| 22 | First playable system | `FlyController` (engine-provided), so scenes are playable without host code |
| 23 | `ohao_game` | Becomes a scene file played by `LiveHost` |

## Frank actions

Only Frank can do these; each step that needs one says so and continues without it where
it can:
1. Approve the project MCP server in Claude Code once (`.mcp.json`, step 11) for the
   manual acceptance run.
2. Apply skill text changes under `.claude/skills/` (the `verify` skill gains
   `python tools/check.py`; step 2 writes the new text into its change record).
3. Rule on any budget a step measures and misses (none expected; the process is spec 3's).

## Open decisions

None.
