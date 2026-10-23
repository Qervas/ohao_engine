---
module: reflection
id: cli
title: The ohao CLI & render host
standard: v2
---

## A program whose stdout is a protocol

`ohao` is the session API as a process: `schema`, `validate`, `fmt`, `apply`, `query`, `summary`, `run` and, for a whole session over JSON-RPC/MCP, `serve`. Its caller is usually a script or an agent that parses stdout, so its contract is strict. Each invocation prints one JSON document, `{"result": …}` or `{"error": {code, message, data}}`. `run --script` prints one JSON line per operation instead. Logs never appear on stdout, and the exit code gives the class of failure.

The engine's own messages already stay off stdout: they go through the logger, whose default sink writes to stderr. Third-party code is another matter (Jolt, NRD, assimp and the driver can all print), so the first thing `main` does is redirect at the descriptor level:

{{cite ohao/rpc/transport.cpp "const int copy = _dup(_fileno(stdout));"}}

The real stdout is duplicated into a private descriptor, and descriptor 1 is then pointed at stderr with `_dup2`. Every later `printf` or `std::cout` from anywhere in the process lands on stderr (on Windows the console handle too: `_dup2` updates it), and the CLI writes its JSON through a `FILE*` opened on the private copy; `ohao serve` writes its protocol messages through the same copy. That copy is set to binary first: a duplicated descriptor keeps text mode, and the step-9 record lists the resulting `\r\n` line endings as one of the bugs found while building the CLI. `emit` flushes after every document, so a consumer reading line by line sees each operation as it finishes. If the duplication itself fails, nothing is redirected, and logs share stdout with the JSON.

## Exit classes

`exitFor` maps an error code to one of four exit codes:

| Exit | Meaning | Codes |
|---|---|---|
| 0 | success | — |
| 1 | validation, usage or input error | every code not listed below |
| 2 | asset or instantiate error | `E_ASSET_NOT_FOUND`, `E_ASSET_INVALID`, `E_MESH_INVALID` |
| 3 | GPU or internal error | `E_RENDER`, `E_INTERNAL` |

`E_INTERNAL` only comes from `main`'s catch handlers, for an exception that escaped. `validate` is a special case. A scene with errors is still a successful `scene.validate` reply, so the CLI looks inside it, and exits 2 only when *every* error is an asset error. A scene with one bad field and one missing model exits 1:

{{cite tools/ohao/main.cpp "return onlyAssets ? kAsset : kInput;"}}

## What `run` does, in order

`run` checks every option before it loads anything. `--dt` without `--steps`, render options without `--render`, a malformed `--size WxH`, and a `--dt` or `--exposure` that is not a JSON number are all usage errors, exit 1. The image size limits and the ranges of `--dt` and `--exposure` are left to the session, so the CLI and the API cannot disagree about them. Then it runs `scene.load` (with `--seed`), `world.step` (`--steps`, `--dt`), the script lines, `render.capture` (`--render`) and `scene.save` (`--save`), each one a `Session::call`.

Without a script, the results are collected into one document, `{"result": {"scene.load": …, "world.step": …, …}}`. The first failure stops the run and prints only `{"method": …, "error": …}`; the results of the operations before it are not printed. With a script, every line runs and prints its own line, `--render` and `--save` still run after a failed line, and the exit code is the worst one seen:

{{cite tools/ohao/main.cpp "const auto step = [&](const std::string& method, const std::string& reply) {"}}

## Script lines are RPC requests without the envelope

A script line is `{"method": "...", "params": {...}}`. The spec defines this as a JSON-RPC 2.0 request without `jsonrpc` and `id`, so a script can be replayed unchanged over the planned `ohao serve` transport. The CLI parses each line, writes the params back out as text, and passes them to the same dispatch every other caller uses:

{{cite tools/ohao/main.cpp "step(std::string(method->text), c.session->call(method->text, ptext));"}}

Blank lines are skipped. A line that is not an object with a string `method` is reported as `E_COMMAND`, "script line N is not …", printed with an empty method name, and the run continues. The `run_script_mistakes` golden pins the output for a misspelled field, an out-of-range value, an unknown snapshot, an unknown method (`world.spin`, suggestion `world.step`) and a line that is not JSON, followed by a query that still succeeds.

## Teardown: why the RenderHost comes before the World

`validate`, `apply`, `query`, `summary` and `run` each build one `Context`, which holds a file resolver, a `RenderHost`, a `World` and a `Session`. The order is load-bearing:

{{cite tools/ohao/main.cpp "std::unique_ptr<host::RenderHost> render;   // before the world: it must outlive it"}}

Once a capture has run, the world holds the render extract and instantiate systems, and their destructors release instance slots, lights and meshes through the renderer's `RenderScene`. The world must therefore die while the renderer is alive. Members are destroyed in reverse order of declaration, so declaring the host first already gives that order. `~Context` also resets the members explicitly: the session (which holds `World&`), then the world, then the renderer.

## The renderer starts on first capture

`schema`, `validate`, `fmt`, `apply`, `query`, `summary` and a `run` without `--render` never create a Vulkan device. The step-9 record shows what that saves: `ohao validate` on the 10k-entity bench scene took 43.0 ms of process time, while loading the Cornell box and rendering one deferred frame took 481 ms, including starting the renderer.

The first `capture` creates the `VulkanRenderer` at the requested size. If initialization fails, the host remembers it and every later capture fails at once with `E_RENDER`. On success it reads the default exposure from the deferred post-processing chain, then installs the render extract and instantiate into the world the session already loaded:

{{cite tools/ohao/render_host.cpp "ecs::installInstantiate(*m_world, m_renderer->renderScene(), m_renderer->getTextureManager(), m_assets);"}}

Both systems tag the entities that already exist when they are installed (see the instantiate page), so a scene loaded on the CPU renders completely on its first frame. Later captures call `resize`, which does nothing at the same size.

## One capture

`RenderHost` is the session's `api::Host` (kind `cli` or `headless`); its `capture` brings the renderer up and then hands the request to `runtime::captureFrame` in `ohao/runtime/frame_capture.cpp`, the one capture procedure the window's `LiveHost` also runs. Each capture applies the world's `Camera` and `Environment`, then sets the mode and the denoiser. In deferred mode it also sets bloom, TAA and SSAO, each on or off according to `post`, and sets the exposure to the requested value or to the default it read at startup. These post settings persist inside the renderer, so they are all set on every capture. Otherwise a capture with `post: ["taa"]` or `exposure: 1` would leak into the next capture that asked for neither. The default exposure is the renderer's own 0.6, set in `initializeDeferredRenderer`. `PostProcessingPipeline`'s member initializer of 1.0 never survives startup, and the step-9 record lists assuming 1.0 as one of the bugs it fixed.

Without `frames`, the frame count is one per path-tracer sample, and for deferred one frame, or ten when TAA is on and blends a history:

{{cite ohao/runtime/frame_capture.cpp "if (r.mode != "deferred") return r.spp;   // the path tracer: one sample per frame"}}
{{cite ohao/runtime/frame_capture.cpp "return std::find(r.post.begin(), r.post.end(), "taa") != r.post.end() ? 10 : 1;"}}

There is no `+ 3`. The examples need it because `getPixels()` delivers the frame from `MAX_FRAMES_IN_FLIGHT` calls earlier; a capture asks for its last frame with `requestReadback()` and takes exactly that frame from `readback()`. The CPU denoisers (OIDN, NRD) read the live images back themselves through `getPixelSpan()`.

{{cite ohao/runtime/frame_capture.cpp "if (i == frames - 1) renderer.requestReadback();"}}

Each frame runs `world.sync()` and then `render()`: the sync phase only, so `spin` does not turn anything during a capture and the session's frame counter does not move. After the first frame's sync, any entity carrying `InstantiateError` turns the capture into an error that names the entity, before any pixel is rendered. A missing model therefore exits 2, not 3. The image is then cropped to `region` if one was given, written as a PNG if a path was given, and encoded as an inline preview if one was asked for. A path that cannot be written is `E_RENDER`, which the `render_unwritable` case uses to cover exit 3.

A second capture in the same process must not inherit the first one's history:

{{cite tools/ohao/render_host.cpp "settings.reset = m_captures > 0;   // the first capture renders exactly as a fresh renderer does (the goldens)"}}
{{cite ohao/runtime/frame_capture.cpp "if (settings.reset) renderer.resetAccumulation();"}}

For the path tracers this restarts accumulation. For deferred, `resetAccumulation` idles the device and sets the frame slot (which also indexes TAA's jitter) back to 0. It then calls `DeferredRenderer::resetTemporalState()`, which zeroes the clock, sets the previous view-projection to the current one, and clears the RT GI and post-processing histories. Before that change, the step-9 record says, a deferred capture after a restore differed by up to 14 levels from a fresh render. The first capture skips the reset, so it renders exactly as a fresh renderer does. That is what the goldens need. The window's `LiveHost`, whose renderer has always been presenting, resets on every lockstep capture.

## Reproducing the examples' goldens

Five `cli_*` entries in the golden manifest render the checked-in scene files through `ohao run` and compare them with the *examples'* goldens, using the same tolerances. Sharing the golden means the scene file and the code-built scene cannot drift apart unnoticed. A scene file holds no render settings, so a capture that has to match `model_viewer` states them on the command line:

{{cite tests/golden/manifest.json "run assets/scenes/damaged_helmet.scene.json --render {out} --mode deferred --frames 27"}}

That entry adds `--post taa,ssao,bloom --exposure 1`, which are the settings `model_viewer` turns on in code. The frame counts are the examples' minus the ring depth: `model_viewer` renders 30 deferred frames and its lagging readback writes frame 27, `cornell_box` renders 10 and writes frame 7, so the deferred entries pass `--frames 27` and `--frames 7`. The RT entry passes `--spp 16`, which renders the 16 frames that `cornell_box`'s `16 + 3` loop delivers. The step-9 record reports the largest per-channel difference against the examples' goldens as 0 or 1 for four of the entries, and 4 for `cli_cornell_box_rt_realtime`.

## Git Bash and `--seed`

Git Bash (MSYS) rewrites any argument that begins with `/` into a Windows path before the program starts. The agent guide gives the example that `/Transform` arrives as `C:/Program Files/Git/Transform`. The program cannot undo that, so `--fields` accepts the form without the slash and adds it back:

{{cite tools/ohao/main.cpp "if (!f.empty() && f[0] != '/') f.insert(f.begin(), '/');"}}

The golden case `query_fields_without_slash` must produce output byte-identical to `query`. Only `--fields` is affected; JSON inside command files and scripts is never rewritten by the shell.

`--seed S` (16 hex digits) is passed to `scene.load`, which seeds the id generator after a successful load. Generated ids are `splitmix64(seed + counter++)`, so the ids that `entity.create` and `entity.duplicate` hand out repeat from run to run. In the `run_script` golden, seed `0000000000000007` creates `63cbe1e459320dd7` every time. Without a seed, the generator is seeded from `std::random_device` and created ids differ on every run.

## How the CLI goldens test it

`tests/cli/check_cli.py` runs the 29 cases in `cases.json`, each twice, from the repo root and from an empty `build/cli_tmp` (a `*_back` case keeps the previous case's files, which it reads):

{{cite tests/cli/check_cli.py "second = run_once(case, exe)"}}

Both runs must agree on the exit code, the stdout bytes and every written file. Stdout must also match `tests/cli/expected/<name>.out`, and each file listed under `files` must match its checked-in reference byte for byte. Two of those comparisons are round trips. `run_steps` steps the Cornell box 30 frames at `dt` 0.02 and saves, and the result must equal `assets/scenes/cornell_box.scene.json`. `run_script` snapshots, edits, steps, restores and saves, and the result must again equal the file it loaded. Cases that print render times are marked `timed`; for those, the two runs need only agree on listed substrings such as `"validation":0`, not on exact stdout. `--cpu-only` skips the four cases that create a device, and the remaining cases still cover exits 0, 1 and 2. `.gitattributes` keeps `tests/cli/**` at LF, so the expected output checks out as the CLI writes it.
