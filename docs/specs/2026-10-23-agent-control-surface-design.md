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

*Correction (step 7).* Time does not go through `Host` virtuals: it is a value both sides
share. The session answers `time.get`/`time.set` from a `Timeline` (see "Time modes", as
built), its own until the host hands it one with `Session::setTimeline(Timeline&)`, and the
host kind (`setHostKind`) decides whether realtime is allowed. One object then holds the
single frame axis, with no copy to keep in step. `Host`'s remaining part (capture) arrives
with the live host (step 9).

*Correction (step 8).* There is no `beforeSteps`: input stamping needs no host virtual.
Every simulation step goes through one function, `Session::step(deviceEvents, dt)`, which
feeds the step's device events and then the injected ones due at that step to the
World's `InputMap`, writes them to a recording, simulates, and advances the frame. The
window host (step 9) calls it once per realtime step with the events polled since the
previous step; `world.step` and `input.replay` call it with none.

*As built (step 9).* `api::Host` is `{kind(), capture(const RenderRequest&)}` and
`Session::setHost(Host*)` replaces `setRenderHandler` (and sets the host kind;
`setHostKind` stays for sessions without a host). The headless `host::RenderHost`
(`tools/ohao`) and the window's `runtime::LiveHost` implement it; `ohao_runtime` now
links `ohao_api`. `Session::changeCount()` counts the calls that may change the world
(`scene.load`, `world.apply`, `world.step`, `world.restore`, `input.replay`) and
`Session::step`, so a window host knows when a lockstep frame is due.

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

*As built (step 5, 2026-10-23).* The params structs live in their own registry,
`api::paramsRegistry()` (`ohao/api/params.{hpp,cpp}`), not the engine's: they are not scene
types, so the scene JSON Schema and the catalogue's type list are unchanged, and each
method's params appear in the catalogue's method table as a self-contained JSON Schema
(`reflect::writeParamsSchema`: no `$ref`, `additionalProperties: false`, defaults, limits,
`required`). The free-JSON kind is `Kind::Json` holding a `reflect::JsonRef`, a pointer
into the request's document, not an owning `JsonValue`: no copy of a 100-command batch
(`world.apply` 100 `field.set` stays at 0.06 ms in-process). Also added to reflection:
`Kind::Int64` (a JSON integer; `log.read`'s `since`), `.required()` (schema `required`,
checked by `decodeParams` as `E_COMMAND`), limits on optionals and vectors of numbers,
and `readStruct`'s ignored key (MCP's `_meta`). `Session::call` gains an overload over an
already parsed request returning `{status: Ok | UnknownMethod | BadParams | Failed |
Cancelled, result, errors}`; errors in the params struct (or a rule on the params alone)
are `BadParams`, which the server maps to -32602. `api::exitClass` (shared with the CLI)
gives a domain error's code. The full `Host` interface is not built yet:
`Session::setHostKind` reports the host in `engine.info` and `world.info`, and
`render.capture` still goes through `setRenderHandler`; time control comes in step 7.

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
  *As built (step 10):* `rpc::PipeTransport` (`ohao/rpc/transport.cpp`, Windows only;
  elsewhere `--pipe` is a usage error), in both hosts. The pipe is created before the window
  or the scene, so a bad or taken name fails at once (exit 1, `E_COMMAND` on stdout). The
  server's reader thread waits for the client in its first read (overlapped I/O; `stop()`
  ends the wait, so closing a window nobody connected to works). **One client per
  process:** its disconnect is the end of input, as EOF on stdin is (the session ends,
  exit 0); stdin is not read. `NAME` is 1–64 of `[A-Za-z0-9._-]`. On exit with the client
  still connected the server flushes the pipe before disconnecting.
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

*As built (step 5).* `ohao/rpc` (`ohao_rpc`, CPU-only): `LineReader` (newline framing,
`\r` dropped, an oversize line skipped without being kept), `StdioTransport` (stdin read
raw; `separateStdout()` points descriptor 1 at stderr and keeps a private binary copy,
which on Windows also redirects the console handle), and `Server`. The reader thread
answers `ping` and handles `notifications/cancelled` itself; every other message,
including the replies to unparseable or invalid lines, is queued and answered by the
engine thread in arrival order, so a scripted session's stdout is deterministic. End of
input cancels the running request **and drops the queued ones** (nobody is left to
answer), then `run()` returns and `ohao serve` exits 0: a client waits for its last reply
before closing stdin. A request id is echoed as its raw text (strings with their escapes,
numbers beyond 64 bits). `notifications/progress` is sent at most every 250 ms, so a
short step sends none. An unknown tool is -32601 with a suggestion, as specified (MCP
itself uses -32602; OHAO keeps one code for "no such method"). `tools/list` is 7,811
bytes for the 14 tools. `logging/setLevel` installs the RPC log sink and lowers the
capture threshold if needed; MCP's `notice` maps to warn, `critical`/`alert`/`emergency`
to error.

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

*As built (step 9, 2026-10-23).* `runtime::LiveHost` (`ohao/runtime/live_host.{hpp,cpp}`)
replaces `Application` (deleted). It owns the window, the renderer, the asset resolver,
the World (instantiate and the render extract installed) and an `api::Session` sharing
its `Timeline`. Per frame: poll the window; pump the requests (a `RequestPump`: the
server's `poll(wait)` runs what is queued on this thread, waiting up to 16 ms while
nothing steps, 0 in realtime); the timeline's steps, each `Session::step(device events,
fixedDt)` and then the game's `fixedUpdate` hook (it reads the step's input; the spec's
"before the step" slot is gone because `Session::step` is one call); the `update` hook;
`World::sync`, the world's view (`applySceneView`) and render + present, every frame in
realtime and in lockstep or paused only when `Session::changeCount()` moved or the
window resized; then `flush`, which sends the replies held since the poll: a reply is
sent after the frame its request caused was presented. While nothing steps, the window's
device events are dropped and the next step starts with a focus loss, so a lockstep
world only sees injected input and a key let go meanwhile is never left held. Captures
run inside the poll, synchronously (the encode is not moved off-thread: it is a few
milliseconds, and the reply waits for it anyway). `rpc::Server` gained `start`,
`poll(wait)`, `flush` and `stop` (`run` is `start`, `poll` until the input ends, `stop`)
and `ServerOptions::holdReplies`; `stop` interrupts a read the client left open
(`Transport::interruptRead`: `CancelSynchronousIo` on Windows), so closing the window
ends the session too. The reader thread is named `ohao rpc reader` (serve_bench finds it).
`ohao_game` loads `assets/scenes/helmet_game.scene.json` (the helmet room, the spinning
cube as a `Spin`, the camera with a `FlyController`, an `InputActions` entity with WASD,
Q/E, right mouse to look, Shift, Escape as `quit`) and keeps only host settings in code
(post effects on, exposure 1; quit and cursor capture in its hooks).

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

*As built (step 7, 2026-10-23).* The phase is in the method and the system's type:
`World::addSimulate(name, fn(World&, float dt))`, and `addSync`, `addSyncBefore`,
`addSyncAfter` with `fn(World&)`, so a sync system cannot take a dt. `simulateNames()` and
`syncNames()` list them. spin is a simulate system; instantiate, transform and the render
extract are sync systems. Headless renders (the examples, `ohao run`'s capture) call
`World::sync()` before each frame, never `update`, so a capture never steps the world.
`world.step` runs `sync` once after its steps, and also when cancelled, so the derived state
matches the last whole step.

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

*As built (step 7).* The time model is `ohao::Timeline` (`ohao/core/timeline.hpp`, with
`FixedStepClock` moved there from `ohao/runtime`): mode, paused, the fixed step in integer
nanoseconds, the frame number and an optional fps. `advance(wall)` returns the fixed steps
due only when `running()` (realtime and not paused); otherwise none, and the wall time is
dropped. Pausing, resuming and switching mode reset the clock's leftover time, so resuming
never catches up on the pause and never runs a step early. The host reports the steps it
runs with `stepped(n)`. It is CPU-only and reads no clock (the host passes wall time in),
so it is tested without a window (`runtime_tests` `sp4_t11_*`). `Application` steps its
World with one (realtime by default, `AppConfig::timeMode`): per frame, for each step due,
the `fixedUpdate` hook and `World::simulate(fixedDt)`, then the `update` hook and
`World::sync()`. Spin therefore runs per fixed step in `ohao_game` too (it ran with the
frame's dt before). A `Session` owns a lockstep `Timeline` until a host shares its own
(`Session::setTimeline`), so the window host's realtime steps and an agent's `world.step`
count on one frame axis; that is the window host's part of step 9. Rules: `time.set {mode:
realtime}` is `E_STATE` unless the session's host is a window host; `world.step` is
`E_STATE` while the timeline is running; `fixedDt` must be in (0, 1]; `paused` may be set
in lockstep (it changes nothing there). The default fixed step, 16,666,667 ns, reads back
as exactly `1/60` in float, so the `world.step` default `dt` did not change.

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

### Input, as built (step 8, 2026-10-23)

- **Where it lives.** `ohao/core/input.{hpp,cpp}` (namespace `ohao`, moved from
  `ohao/runtime`). The `World` owns the `InputMap` (`World::input()`) and steps it with
  `World::stepInput(events)` once per simulation step; `Application::input()` returns the
  World's. `Application` collects the events polled during a frame and hands them to the
  first step that runs, so a device event is stamped with the step that consumes it.
- **Key names** are the reflected enum `Key` (`ohao/core/input`, registered as `"Key"` in
  the catalogue), its values the GLFW codes, mouse buttons at `1000 + button`. Names are
  identifiers, so digits are `Digit0`–`Digit9`; `None` (0) is "unbound" and is not a key
  `input.inject` can press. `ActionBinding::key` and `AxisBinding::positive`/`negative`
  are `Key`, not `std::string` as sketched above, so the scene codec gives `E_ENUM` with a
  suggestion for free, in files and commands alike (corpus case `20_input`).
- **Rebinding.** `InputActions` construction, update and removal set a flag (an EnTT
  signal); the next `stepInput` rebuilds the bindings from the singleton (an axis side
  `None` is no key). `InputMap::beginStep` re-evaluates every action and axis from the
  current device and virtual state before applying the step's events, so a rebinding
  with no new event takes effect at the next step (a bug found here: before, a removed
  binding left its action held until some key changed).
- **The virtual device.** `InputEvent` gains `ActionDown`/`ActionUp`, `AxisSet`/
  `AxisRelease` (`value: null`) and `MouseDelta`. An action is held while a bound key *or*
  the virtual device holds it; an injected axis value overrides its keys until released.
  `FocusLost` releases keys, mouse buttons and the virtual device alike.
- **`input.inject`** validates every event (exactly one of `action`, `axis`, `key`,
  `mouse`, `focusLost`; `down` for actions and keys; `value` for axes) and queues them by
  absolute step; same-step events keep their order, this call's after earlier calls'.
  `world.snapshot` restore and `scene.load replace` drop the queue (its steps refer to a
  world that is gone). Nothing else rejects an injection: an action name that nothing
  binds is still a held action, readable through `input.state`.
- **Recording.** The header has `format`, `version`, `schemaHash`, the last `scene.load`'s
  `scene` and `seed` when there was one, `startFrame`, `startHash`, `fixedDt`. Then each
  step's events (`key` by name, or `keyCode`/`button` for a code with no name; `cursor` for
  absolute pointer moves; `mouse` for deltas), a `{"step", "dt"}` line before any step
  whose length differs from `fixedDt`, `{"step", "hash"}` after every 60th step
  (the hash of the world *at* that step, before it runs), and `{"end", "hash"}` on stop.
  Starting a recording releases everything held, as a focus loss, so a recording and its
  replay start from the same input state.
- **Replay.** Refused (`E_STATE`) while the timeline is running or while injected events
  are queued (both would mix other input into the replayed steps). With `verify` (the
  default), the world's hash must equal `startHash`, each checkpoint is compared before
  its step runs, and `E_REPLAY`'s `expected` names the step of the first mismatch; the
  world is left there, synced. The recording's steps run from the current frame on (its
  step numbers order it), and a replay also starts by releasing everything. `verify:
  false` runs it without hashing. A replay is recorded like any other steps.
- **`FlyController`** turns while `look` is held by the mouse delta:
  `rotation = normalize(yaw(-dx·rate) · rotation · pitch(-dy·rate))`, `rate =
  lookDegPerUnit` in radians (yaw about world up, pitch about the local X); then moves by
  `(front·forward + right·right + up·up) · moveSpeed · (fast ? fastMultiplier : 1) · dt`
  with `front = rotation · -Z`, `right = rotation · +X`, `up` = world Y. It patches the
  `Transform` only when it moved or turned. `ohao_game`'s quit and fly-camera code moved
  into its `fixedUpdate` hook (per step, reading the World's map); the scene-file
  `ohao_game` and `helmet_game.scene.json` are step 9's, with `LiveHost`.
- **`world.hash` moves when types are added.** The hash covers every saved type's column,
  so adding `InputActions` and `FlyController` changed the hash of unchanged scenes (the
  CLI's rooms goldens: `0dc30d7b47b1f234` → `87ba2c301e031a40`). Hashes compare worlds of
  one engine build, as recordings do through `schemaHash`.

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

*As built (step 9, 2026-10-23).*
- **One procedure.** `runtime::captureFrame` (`ohao/runtime/frame_capture.cpp`) is what
  both hosts run: the world's Camera and Environment, the mode, denoiser, post effects and
  exposure, a temporal reset (except on a renderer that has rendered nothing), `frames`
  frames each after `World::sync()`, the last one read back on request, then the region,
  the PNG file and the preview. A lockstep window capture therefore equals a headless
  capture of the same world at the same size by construction, and the test shows it bit
  for bit with post effects off and on. A lockstep capture keeps the window's own post
  settings (it puts them back after) and leaves its last frame on screen.
- **Frames.** Defaults: deferred 1, or 10 when TAA is on (as specified); the path tracer
  `spp`, not `spp + 3`. *Correction:* the `+3` compensated a lag: the headless renderer
  copies every frame into its slot's staging buffer and `getPixelSpan()` returns the copy
  made `MAX_FRAMES_IN_FLIGHT` (3) frames earlier, so the examples' 10-frame deferred
  images are frame 7 (and a capture of 3 frames or fewer was black). A capture now keeps
  the frame it rendered last; the deferred CLI goldens state `--frames 7` and
  `--frames 27` (the frames the examples write), and the path-traced ones are unchanged
  at `spp` frames. *Correction:* "no temporal effect" is not true of the hybrid: RT GI
  keeps a history, so frame 1 and frame 10 of Cornell differ (max_abs 29, 8% of pixels
  above 4). A converged image needs `frames`.
- **Readback on request.** `VulkanRenderer::requestReadback()` marks the next frame;
  `readback()` waits for that frame's fence only and returns its pixels from the frame
  slot's persistently mapped staging buffer. A window frame copies its final image there
  (inside the swapchain blit, the image already a transfer source) only when asked;
  `readbackCopyCount()` still counts only the headless ring (0 in a window) and
  `requestedReadbackCount()` the asked-for copies. The staging buffers are now
  host-cached memory when the device offers it coherent: the CPU read of one 1280x720
  frame from uncached memory took ~20 ms, and every headless frame paid it (a one-frame
  headless capture went from ~45 ms of CPU to ~1 ms).
- **Params and result.** `path` is optional; `inline`, `format` (`jpeg` | `png`),
  `maxSize` (default 768) and `region` are added; `width`/`height` are optional (headless
  1920x1080; a window host its own size, another is `E_COMMAND`). The result is
  `{path?, width, height, imageSize?, frames, ms, readbackMs, encodeMs, deterministic,
  validation?}` and the image travels beside it (`api::CallOutcome::image`): an MCP
  `image` content block, or natively `"image"` (base64) and `"mimeType"` in the result.
- **A window host captures deferred only.** *Correction:* the path tracer does not
  present (its frames have no swapchain path), so a window host answers a path-traced
  capture with `E_COMMAND`; a headless host renders it.
- **PNG and JPEG.** *Ruling (ledgered):* stb_image_write's PNG took 7.3 s for one
  1280x720 Cornell frame, so the capture path writes PNG with its own encoder
  (`ohao/runtime/image.cpp`): filter Up, the fixed Huffman codes with runs as distance-1
  matches, four bands of rows deflated on four threads (each byte-aligned by an empty
  stored block; Adler-32 combined), and stored blocks for a band that does not compress.
  fpng, the spec's named fallback, would need the network and a new dependency; this
  needs neither, and `runtime_tests` proves it lossless through stb_image's decoder.
  Measured on synthetic 1280x720 frames: 4.1 ms (mixed), 6.1 ms (all noise). The preview
  is an area downscale (3 ms for 1280x720 to 768x432) and stb's JPEG at quality 85
  (4 ms).

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

*As built (step 7): `world.hash`.* The content is what the spec names, read differently for
speed (the first draft, a snapshot then a pass over it, measured 0.86 ms of the 1.0 ms
budget):
- **Read live, in snapshot order, no copy.** `ecs::hashWorld` captures only the entity
  table (`captureEntityTable`: ids, creation sequence, parent rows, child rows in
  `Children` order, the id generator), then hashes each saved component type straight from
  its EnTT storage: which rows have it, then its values in row order. Packed storage order
  is never hashed (it depends on the pools' history, not on the state), so a restored world
  and a loaded one hash alike.
- **Values field by field** (`reflect::hashStruct`, a typed `FieldOps::hash` per member):
  floats by their bits (as equality: -0 ≠ +0), strings by length and bytes, optionals by
  presence, variants by index, entity references by the target's id, never a handle,
  padding or heap pointer. A type whose registered fields add up to its whole size (only
  numbers, vectors, quaternions, matrices and structs of them; `Transform`,
  `MaterialOverride`, most primitives) is hashed as its bytes, which are then exactly its
  field bits; decided once per type (`TypeInfo::plainBytes`).
- **The function** is FNV-1a 64 taken over 8-byte words (`reflect::Hasher`), with a
  xor-shift fold after each multiply and a splitmix64 finish. Each word's step is a
  bijection of the state, so inputs that differ in one word always hash differently; a word
  per step is eight times faster than byte-wise FNV.
- The frame number is not hashed (`world.hash` reports it beside the hash): two worlds in the
  same state at different frames hash alike.
Measured on bench10k: 0.52 ms over stdio (0.09 ms of it the entity table).

*As built (step 7): `world.diff`.* `ecs::diffWorld` matches the snapshot's rows to live
entities (fast path: row r at packed position r, else by id), diffs each column with a
typed, read-only pass (`diffColumn`: a `memcmp` first for trivially copyable components,
then bit-exact per field) and compares parents by id. The API turns it into
`"/Type/field": [old, new]` in file form, `"/Type": [old|null, new|null]` for a component
added or removed (file form, defaults left out), `"/Parent/entity"` or `"/Parent"` for the
hierarchy, and `created`/`destroyed` ids (creation order). `fields` keeps only the listed
types or fields, and only entities with a listed change count. `limit` caps each of
`changed`, `created` and `destroyed`; `count` counts changed entities, `more` everything
left out. Children order and creation sequence are not compared (the file form has neither
as a field). Measured: 0.44 ms over stdio, bench10k with 100 entities changed.

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
  *As built (step 10):* the DACL is protected (`D:P(A;;GA;;;<user SID>)`: no inherited entry,
  no default read access for Everyone). `FILE_FLAG_FIRST_PIPE_INSTANCE` makes a name that
  another process already serves an error rather than a second instance beside a squatter.
  Measured: a second client gets `ERROR_PIPE_BUSY` (231); a client through
  `\\localhost\pipe\...` (the SMB redirector, remote to the pipe) gets `ERROR_ACCESS_DENIED`
  (5), and a control pipe with the same DACL but without the flag admits it, so the
  refusal is the flag's.
- **File sandbox.** Paths in params (`scene.load`, `scene.save`, `render.capture`,
  `input.record`, `input.replay`) are project-relative with `/`, resolved by the spec-3
  resolver rules (no absolute paths, no `..`). Reads are confined to `--root` (default:
  the working directory); writes to `--out` (default `<root>/agent_out/`, git-ignored).
  Anything else is `E_ACCESS`. `--allow-path DIR` (repeatable) adds a root explicitly.
  *As built (step 5):* `api::PathPolicy` / `resolvePath` (`ohao/api/paths.{hpp,cpp}`),
  set by `ohao serve` with `Session::setPathPolicy`; the CLI keeps paths as given. A
  relative path resolves against the root and may not contain `..` or `\`; reads stay in
  the root, writes in `--out` (created, and a write's parent directories with it); an
  absolute path is admitted only inside an `--allow-path` root, which admits reads and
  writes. Containment is element by element on canonical paths (a sibling directory that
  shares a root's prefix is outside it). Results echo the path as given. `agent_out/` is
  git-ignored.
- **No code execution.** No method runs a shell, loads a library or evaluates script in 4.
- **Bounded work.** Message size, batch size (10,000 commands), `world.step` frames
  (1,000,000 per call) and log page sizes are capped; long work is cancellable.
  *As built (step 10):* message size 64 MiB (`--max-message`), `world.step` 1,000,000
  frames, `log.read` 1,000 lines, `world.query` 100,000 entities (step 5); `world.apply`
  10,000 commands and, added here, `input.inject` 10,000 events a call, both as a
  `maxItems` field attribute in reflection: counted before any element is decoded
  (`E_RANGE` at the member, "got: N elements") and given as `maxItems` in the params
  schema an agent reads (so `schemaHash` changed). Also added: at most 100,000 injected
  events wait for their steps at once (`E_STATE`: step first), and the server's queue of
  messages read but not yet run is capped at 256 MiB (`ServerOptions::maxQueued`): at the
  cap the reader stops reading until the engine thread catches up, so a client flooding a
  busy engine blocks on its own writes instead of growing the server's memory (ping waits
  too then).
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

*As built (step 6), the CPU part.* `serve_bench` builds bench10k (seed 2026), saves it
under `build/serve_bench/`, loads it both into an in-process `Session` and into
`ohao serve` started as a child process on pipes (high priority, `--log-level warn`), and
gives the first 100 entities a `Spin`. Each round runs every operation as one block
(20 calls; 10 for `world.apply` and `world.step`; 10^6 log calls below the threshold,
20,000 into the ring), operations interleaved, 5 warm-up rounds then 50; the transport
overhead is each round's RPC median minus its in-process median. Server page faults per
call are reported beside every RPC median. After timing it checks the replies (10,002
entities, `count` 1, `applied` 100, a `frame`) and that the server exits 0 when stdin
closes, so a bench of broken code fails rather than timing errors. The load monitor
excludes the server it started. Five consecutive passes on 2026-10-23 (medians, ranges
over the five): `world.info` round trip 0.0179–0.0196 ms, transport overhead
0.0176–0.0203 ms, `world.query` 0.0186–0.0216 ms, `world.apply` 100 0.0888–0.0955 ms,
`world.step` 0.0374–0.0397 ms, `OHAO_LOG` below the threshold 0.195 ns, into the ring
193.9–195.8 ns, `tools/list` 7,811 B. Recorded: start to `initialize` 18.7–20.2 ms,
`scene.load` bench10k over stdio 27.4–30.0 ms, `initialize` reply 725 B (`instructions`
551 B).

*As built (step 7).* Two more ops: `world_hash_rpc` (10 calls per block) and
`world_diff_1pct_rpc` (10 calls of `world.diff {snapshot: "diff_base"}`). The diff's state is
set up once over stdio (snapshot `diff_base`, `field.set` of the position of 100 entities,
snapshot `diff_now`), and each diff block first restores `diff_now`, untimed (an `Op`'s
`prepare`), since the ops beside it move the world. After timing the bench checks that two
hashes agree and that the diff counts the 100 entities. Five consecutive passes on
2026-10-23: `world.hash` 0.5193–0.5251 ms (p95 at most 0.8014), `world.diff` 0.4373–0.4541
ms (p95 at most 0.6831); the step 6 ops stayed where they were (`world.info` 0.0203–0.0209,
`world.apply` 0.0952–0.0982, `world.step` 0.0387–0.0409 ms). `tools/list` is 10,357 B for
18 tools.

*As built (step 9), the GPU part.* `serve_bench` starts `ohao serve --window 1280x720
--hidden assets/scenes/cornell_box.scene.json` (lockstep, no post) as a second child and
runs 5 warm-up and 50 timed rounds of five `world.step {frames: 1}` (each reply after its
frame is presented), one `render.capture {inline: true}` and one to a PNG file; the
readback is the captures' `readbackMs` (that frame's fence and the copy out). Then
`time.set realtime`, at least 600 idle steps, and the reader thread's CPU time over them
by `QueryThreadCycleTime` (the thread is found by its name; `GetThreadTimes` counts in
15.625 ms ticks). Its own load record decides `noisy` (the window host is excluded).
Measured on 2026-10-23, every run noisy (UnrealEditor 230–313% of a core, or another
session's python at 85–89%): step to present 0.43–0.56 ms median; PNG capture
16.5–25.2 ms; reader thread 0.078 ms over 604 steps; **readback 5.39–5.44 ms** (8.3 ms
under UnrealEditor's GPU load) **and inline capture 26.8–30.9 ms (55 ms under it), over
their budgets of 2 and 20 ms.** Both are the frame's GPU cost: the renderer's timestamps
put one Cornell deferred frame at 1280x720 at 5.0–5.6 ms (RT shadows at 8 rays and RT GI
at 32 samples a pixel), and a fence wait for a frame cannot be shorter than its GPU time;
the readback itself adds ~0.3 ms. The inline capture in the bench follows five steps
whose frames are still on the GPU (2–3 frames, 10–15 ms of backlog), then renders and
reads back its own; with the GPU idle first it measures 12–15 ms. No budget was changed.
*Frank's ruling (2026-10-24):* `window_readback_ms` 2 and `window_capture_inline_ms` 20
stay as they are, neither redefined nor relaxed; both misses are open items for
sub-project 2b (a cheaper deferred frame).

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
  *As built (step 9):* also `--hidden` (a window that is never shown, for tests; it
  presents); `--window` alone is 1280x720. *Step 10:* `--pipe NAME` in both hosts (see
  "Transport and framing").
- **`ohao doctor [--cpu-only]`** (above).
- **`ohao run`** gains nothing new but runs on the same host interface; its goldens stay.
- **`ohao_game [scene]`**: the live host in realtime with no protocol; default scene
  `assets/scenes/helmet_game.scene.json`. Its flags and the presentation gate are
  unchanged.
- **`serve_bench [--check FILE] [--out FILE] [--reference FILE] [--rounds N] [--cpu-only]
  | --calibrate`**: the new budgets (as built, step 6: results in `build/serve_bench.json`).
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
   - *As built (step 5):* tests 7 and 8 in-process are `rpc_tests` (quick, 10 tests
     `sp4_t07_*`/`sp4_t08_*`, a scripted in-memory transport: byte-by-byte reads, CRLF,
     oversize lines through `LineReader` with a 7-byte chunk, the error table including
     class 3 from a fake renderer, ids, notifications, ping answered during a long step,
     progress, cancelling a running and a queued request, end of input, initialize for
     both versions and an unknown one, `tools/list` coverage and size, `tools/call`
     results and errors, `logging/setLevel`), plus `api_tests`
     `sp4_t07_cancel_leaves_whole_steps` (a cancelled step leaves the world bit-identical to
     the same number of whole steps) and `sp4_t08_params_structs_lint_clean_…`. Tests 8
     (schemas through `jsonschema`), 9, 10 and 15 (paths) run the process:
     `tests/serve/check_serve.py` (quick, 105 checks). Test 8's examples are one per tool
     in the script until the agent guide (step 11) has its own. Test 9's sessions are
     `tests/serve/sessions/{native,errors,mcp}.jsonl` with `expected/` beside them. Test
     10's server is `rpc_print_server` (printf, puts, fprintf, fwrite, fputs, putchar,
     cout, `_write(1)` and `WriteFile` on the console handle; all reached stderr).
11. **Time model:** simulate systems run once per step, sync once per batch; lockstep
    counts are exact; `world.step watch`; `E_STATE` rules per host and mode; `dt` default;
    a realtime-paused-stepped sequence reaches the same hash as pure lockstep.
12. **Hash and diff:** the hash is stable across save → load with the same seed, changes
    on any authoring change and ignores runtime components; `world.diff` equals a diff of
    the two saves, respects `limit`/`more`.
   - *As built (step 7):* test 11 is `runtime_tests` `sp4_t11_*` (3: lockstep ignores wall
     time; 600 realtime frames of one fixed step are 600 steps; pausing, resuming and
     switching mode neither run nor drop a step, and the default fixed step is 1/60 s
     exactly) and `api_tests` `sp4_t11_*` (5: simulate 5 times and sync once for 5 steps,
     exact frame counts over batches; `watch` equals a `world.query` after the step, a bad
     watch path points into `watch` and runs no step; `E_STATE` for realtime on a headless
     host and for `world.step` while a window host's realtime runs, allowed paused and in
     lockstep; the default `dt` is the fixed step and follows `time.set`; a realtime loop
     driven by hand with uneven wall times, a pause, 17 steps while paused and a resume
     reaches the hash of the same number of lockstep steps, and one more step does not).
     Test 12 is `api_tests` `sp4_t12_*` (4: the hash is stable across save → load with the
     same seed, ignores WorldTransforms and dirty tags, and differs for another seed; it
     changes for a one-ulp position, a sign bit (-0), a name, a component added or removed,
     a reparent, a create and a destroy, and comes back on restore; `world.diff` of 200
     entities after 30 mixed edits equals a diff of the two saves (paths and values),
     created and destroyed included, and nothing after a restore; `limit`/`count`/`more`,
     `fields`, an unknown snapshot, a bad projection, a missing `snapshot`) plus
     `reflect_tests` `sp4_t12_value_hash_ignores_padding_and_counts_bits`. The native
     protocol session (`tests/serve/sessions/native.jsonl`) now covers `world.hash` before
     an edit and after the restore (equal), `world.diff`, `time.get`/`time.set` with the
     `E_STATE` refusal, and `world.step` with `watch`.
13. **Input:** key-name table round trip; `InputActions` rebinds on change; injection gives
    `pressed` on exactly the stamped step, `held` after, `released` on the release step;
    axis override and release; `FocusLost`; record → replay matches every checkpoint; a
    tampered recording fails with the right step; a realtime recording with dropped time
    (the `maxSteps` clamp) replays exactly.
14. **`FlyController`:** 60 steps of `move_forward = 1` move the entity by exactly
    `moveSpeed · dt · 60` along its front; look turns by the configured rate; `fast`
    multiplies.
   - *As built (step 8):* test 13 is `runtime_tests` `sp4_t13_*` (3: key names round
     trip, the virtual device acts like a device, a rebind counts at the next step
     without an event) and `api_tests` `sp4_t13_*` (8: injection stamped by step, axis
     override and release, `InputActions` rebinding on change, focus lost, unknown key
     names `E_ENUM` with a suggestion, record → replay over 150 steps with 3 checkpoints,
     a tampered recording failing at step 120, a realtime recording with dropped time
     replaying exactly), plus the corpus case `20_input` and the native protocol session's
     inject/step/state/bad-key requests. Test 14 is `systems_tests` `sp4_t14_*` (3).
15. **Security:** absolute paths, `..`, and writes outside `--out` are `E_ACCESS`;
    `--allow-path` admits exactly its root; a second pipe client is refused; the size
    and step caps hold.
   - *As built (step 5, paths part):* `api_tests` `sp4_t15_paths_are_confined_to_the_sandbox`
     and `check_serve.py`'s sandbox checks, with the message-size cap (`--max-message`)
     and the step cap. The pipe and the remaining caps are step 10's.
   - *As built (step 10, the rest):* `rpc_tests` `sp4_t15_*` (5: the queue cap stops the
     reader and every request is still answered in order, `stop()` releases it; pipe names;
     one local client of the current user — the request answered over the pipe, a second
     client `ERROR_PIPE_BUSY`, a second server on the name refused, the protected
     one-entry DACL read back, the disconnect ending `run()`; remote refusal against a
     control pipe; `stop()` ending a wait for a client), `api_tests`
     `sp4_t15_batch_and_inject_caps_hold`, `reflect_tests`
     `sp4_t15_max_items_is_checked_before_reading`, `check_serve.py`'s caps (7 checks) and
     pipe (12 checks: `ohao serve --pipe` as a process, stdin left open) and `check_live.py`'s
     window host over the pipe (3 checks).

GPU:

16. **Live host:** a test client drives `ohao serve --window` (hidden window allowed):
    load, apply, inject, step, capture, read logs; lockstep capture equals a headless
    capture of the same state bit for bit (deferred, post off and on); 0 validation
    messages through teardown; 0 unrequested readbacks; a disconnect mid-step exits 0 with
    clean validation; capture while minimized is `E_STATE`.
17. **`ohao_game` from a scene:** the presentation gate passes unchanged; the scene file
    saves byte-identically after load; the fly camera responds to injected input as test
    14 says.
   - *As built (step 9):* tests 16 and 17 are `tests/serve/check_live.py` (label `gpu`,
     22 checks, a hidden window: it presents and captures as a visible one does) and the
     unchanged `ohao_game_presentation` CTest (600 frames, a resize, 0 readbacks, 0
     validation messages through teardown, now from the scene file). Also: `api_tests`
     `sp4_t16_*` (2: the inline image beside the result, natively and for MCP, and the
     params rules; `changeCount`), `rpc_tests` `sp4_t16_*` (3: `poll` holds replies until
     `flush` while ping is answered; `stop` interrupts an open read; the MCP image block),
     `runtime_tests` `sp4_t16_*` (2: the PNG writer round trips through stb_image for a
     frame, all noise and odd sizes; crop, the area downscale, the JPEG).
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
     not yet by `serve_bench`, which step 6 builds; step 6 folded them in and deleted
     `log_bench`. `log_bench` already applied the calibration rule (`--calibrate` writes
     `build/serve_bench_reference.json`; >25% slower is `noisy`, exit 2) and, at the
     reviewer's request, recorded every other process that used CPU during the run
     (`tools/bench/bench_env.hpp`): more than 50% of a core in total is also `noisy`.
5. **Protocol core and headless `ohao serve`.** `ohao_rpc` (framing, ordering, errors,
   cancellation, progress), the MCP adapter, method params as reflected structs (the
   catalogue's method table gains schemas; `schemaHash` changes once), `engine.info`,
   `scene.load replace`, the file sandbox, `ohao serve` headless over stdio, protocol
   goldens. Tests 7–10, 15 (paths part).
   - *Done 2026-10-23.* See the as-built notes under "Method params are reflected
     structs", "MCP mapping" and "Security". Also built here: `log.read` (no step named it;
     the logger exists since step 4 and `logging/setLevel` needed the sink), `world.info`'s
     `host`, and the error codes `E_ACCESS`, `E_STATE`, `E_REPLAY` in the catalogue.
     `schemaHash` changed once (CLI, catalogue and serve goldens updated after review).
     `engine.info`'s `time` is lockstep-only until step 7.
6. **`serve_bench`, CPU budgets.** The child-process client, calibration, the CPU budget
   table, `tools/list` size gate. Test 18 (CPU part).
   - *Done 2026-10-23.* See "Budgets", as built. `log_bench` is folded into `serve_bench`
     and deleted; `scene_bench` and `serve_bench` share the bench10k builder
     (`tools/bench/bench_world.hpp`). `tools/check.py` reruns a test whose summary line
     says `"noisy": true` alone, at most twice, and reports every attempt
     (`attempts: [{ok, noisy, seconds}]`); one still noisy after that is not passed and
     keeps `noisy` in `check.json`. `world.hash` and `world.diff` budgets wait for step 7,
     which builds the methods.
7. **Time model, hash, diff.** System phases, `World::simulate`/`sync`, lockstep and
   realtime clocks in `Application`, `time.get`/`time.set`, `world.step watch`,
   `world.hash`, `world.diff`. Tests 11, 12.
   - *Done 2026-10-23.* See the as-built notes under "System phases", "Time modes",
     "Determinism" and "Budgets", and the correction under "The host interface" (time is a
     shared `Timeline`, not `Host` virtuals). `world.info` gains `time`; `schemaHash`
     changed (catalogue, CLI and serve goldens updated after review). The `world.hash` and
     `world.diff` budgets pass five consecutive `serve_bench` runs.
8. **Input as data.** `InputMap` to `ohao/core`, key names, `InputActions`, the virtual
   device, `input.inject`/`state`/`record`/`replay`, `FlyController`. Tests 13, 14.
   - *Done 2026-10-23.* See "Input, as built" and the correction under "The host
     interface" (`Session::step` replaces `beforeSteps`). `schemaHash` changed (catalogue,
     CLI and serve goldens updated after review). Also, at the reviewer's request,
     `scene_bench` gained serve_bench's load record and per-round CPU and memory probes
     (see "Budgets", as built).
9. **Live host and capture.** `LiveHost`, `ohao serve --window`, the request queue on the
   engine thread, lockstep render-on-demand, readback on request, inline previews,
   `ohao_game` on `helmet_game.scene.json`, GPU budgets in `serve_bench`. Tests 16, 17,
   18 (GPU part).
   - *Done 2026-10-23.* See "Live host" and "Frame capture", as built, the host
     interface note, and "Budgets", as built. Two GPU budgets (readback 2 ms, inline capture
     20 ms) miss on the frame's own GPU cost. *Frank's ruling (2026-10-24):* both budgets
     stay exactly as they are; the misses are open items owned by sub-project 2b, which
     makes the deferred frame cheaper (RT GI samples, RT shadows; the deferred goldens may
     change). Steps 10 and 11 do not work on the frame cost.
10. **Named pipe and limits.** `--pipe`, the DACL and remote rejection, single client, the
    remaining caps. Test 15 (rest).
    - *Done 2026-10-24.* See the as-built notes under "Transport and framing" and
      "Security" (the pipe, the DACL, remote rejection, bounded work). Added beyond the
      list: `input.inject`'s 10,000-event and 100,000-waiting caps, the server's 256 MiB
      queue cap, and reflection's `maxItems` (`schemaHash` changed; CLI and serve goldens
      updated after review: only the hash).
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
   *Done for step 9 (2026-10-24):* the two GPU budgets stay; their misses are 2b's.

## Open decisions

None.
