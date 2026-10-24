---
module: agent
id: protocol
title: The protocol server
standard: v2
---

## One server, two kinds of client

`ohao serve` turns the session API into a long-lived process. Its protocol is JSON-RPC 2.0, one compact message per line, and `rpc::Server` answers two kinds of client on the same stream. A native client calls catalogue method names directly (`world.query`). An MCP client sends `initialize`, `tools/list` and `tools/call`, naming tools such as `world_query`. Both reach the `Session::call` overload that takes an already parsed request and returns a status (`Ok`, `UnknownMethod`, `BadParams`, `Failed` or `Cancelled`) instead of a JSON envelope. Nothing in `ohao/rpc` knows about Vulkan, windows or the renderer; a `reflect_tests` lint checks its includes.

## A reader thread and an engine thread

The server runs on two threads. The reader thread splits the input into lines, parses each one and queues it. The engine thread (the one that calls `run()`, or `poll()` in the window host) takes the queue one item at a time, in arrival order, and runs each item to completion. The `Session` is not thread-safe and does not need to be: only one thread ever touches it. The spec states the cost of this choice. A long request (`world.step` with 10,000 frames) delays every request behind it, and that is the price of a deterministic order. An agent that does not want to wait can cancel.

The reader does very little work itself. Even the reply to a line that does not parse is queued (`enqueueReply`) rather than written from the reader thread, so error replies come out in the same order as everything else, and a scripted session's stdout is byte-for-byte reproducible. Request ids are kept as their raw JSON text and echoed back byte for byte: a string with its escapes, or a number with more digits than fits in 64 bits, comes back exactly as it was sent.

Two messages are handled by the reader itself, because their purpose would be lost if they waited behind a busy engine. The first is `ping`:

{{cite ohao/rpc/server.cpp "if (method->text == "ping") {   // answered here, even while the engine is busy"}}

The second is `notifications/cancelled`. The reader looks up the request id. If that request is running, the reader raises the flag the engine thread is polling. If it is still queued, the reader marks the queued item, and the engine thread later drops it without running it:

{{cite ohao/rpc/server.cpp "if (!runningId.empty() && runningId == id) cancelRunning = true;"}}

{{cite ohao/rpc/server.cpp "if (item->cancelled) continue;   // MCP: a cancelled request gets no response"}}

## Cancellation stops at a whole step

A running request sees the flag through `api::CallControl`, a pointer to the atomic plus an optional progress callback. Long methods check it between whole units of work and never in the middle of one. `world.step` and `input.replay` check before each simulation step, and a multi-frame capture checks between frames. A cancelled step loop runs `World::sync()` once, so the derived state matches the last whole step, marks the call cancelled and returns. By MCP's rule a cancelled request gets no response. The agent learns where the world stopped from its next `time.get` or `world.info`. `sp4_t07_cancellation_and_ping_while_busy` runs this sequence: a million-frame step, a queued 5-frame step behind it, a ping answered mid-step, both requests cancelled, then a `world.info` reporting a frame strictly between 0 and 1,000,000.

End of input is handled the same way. When the reader's `LineReader` reports the end of the stream, it clears the queue (nobody is left to read the answers) and cancels the running request:

{{cite ohao/rpc/server.cpp "cancelRunning = true;        // the running request stops at its next whole step"}}

`poll()` then returns false, `run()` returns, and `ohao serve` exits 0. A client must therefore wait for its last reply before closing its end.

## Progress, throttled

A request whose params carry MCP's `_meta.progressToken` gets `notifications/progress {progressToken, progress, total}` while it runs. The session calls the progress callback after every step, and the server drops calls that come less than 250 ms after the last one it sent:

{{cite ohao/rpc/server.cpp "if (now - last < std::chrono::milliseconds(250)) return;"}}

The timer starts with the request, so a request shorter than 250 ms sends no progress. The params decoder skips `_meta`, so it is never an unknown field.

## The error table

Domain errors keep the session's error object. The JSON-RPC envelope adds an integer code, and the code says what kind of error it was:

| Situation | `error.code` | `error.data` |
|---|---|---|
| A line that is not JSON | -32700 (with the column) | none |
| A batch array, a non-object, a bad `id`, `jsonrpc` other than `"2.0"`, a request without a string `method`, a line over `maxMessage` | -32600 | none |
| An unknown method | -32601 | `{code: "E_METHOD", suggestion?}` |
| Params that fail their params struct | -32602 | the diagnostics |
| Any other error | 1, 2 or 3 | `{code: "E_…", diagnostics}` |

Batches are refused on purpose. MCP dropped them in 2025-06-18, and `world.apply` already gives an atomic batch of commands:

{{cite ohao/rpc/server.cpp "batches are not supported; send one message per line"}}

A domain error's integer is the CLI's exit class, from the same function, so a script that switches from `ohao run` to `ohao serve` reads the same classes:

{{cite ohao/rpc/server.cpp "reply(errorLine(item.id, api::exitClass(o.errors.front().code), messageOf(o.errors), w.str()));"}}

`E_RENDER` and `E_INTERNAL` are class 3, asset and mesh errors class 2, everything else 1. An exception that escapes a method becomes class 3, `E_INTERNAL`, and the server keeps serving. `data` carries at most 20 diagnostics, and `message` is the first one's message with ` (+N more)` appended, as in the CLI. A message that has an id but no `method`, and carries a `result` or an `error`, is a response to a request the server never sent, so it is ignored rather than answered with an error.

## MCP on the same server

`initialize` answers with the client's protocol version if the server supports it, and otherwise with the newest one it supports:

{{cite ohao/rpc/server.hpp "inline constexpr std::string_view kMcpVersions[] = {"2025-06-18", "2025-11-25"};"}}

The reply declares two capabilities, `tools` and `logging`, plus `serverInfo` and an `instructions` string. The instructions tell the agent how to orient (`world_summary`), how to read with a projection, edit atomically, step, save under `agent_out/`, look and read logs. The spec caps them at 600 characters, and the build enforces the cap:

{{cite ohao/rpc/server.cpp "static_assert(kInstructions.size() <= 600, "the spec caps the instructions at 600 characters");"}}

`tools/list` has one tool per method in catalogue order. The tool name is the method name with `.` replaced by `_`, because Claude's tool-name pattern rejects dots; the server keeps a map back from tool to method. The description is the method's doc line. The input schema is the method's params struct, written by the same reflection that validates the params, so the schema an agent reads and the check its call goes through cannot disagree:

{{cite ohao/rpc/server.cpp "w.key("inputSchema"); reflect::writeParamsSchema(w, *m.params);"}}

The list is built once and cached. The step-10 record measures it at 14,543 bytes for 22 tools, and `rpc_tests` holds it to the spec's 24 KB budget, so an agent knows in advance how much connecting costs.

`tools/call` maps outcomes onto MCP's two kinds of failure. Params that fail their struct are a protocol error (-32602), as for a native call. An unknown tool is -32601 with an edit-distance suggestion. MCP itself uses -32602 for that case, but OHAO keeps one code for "no such method". A domain error is not a JSON-RPC error at all: it is a tool result whose text block is the error object, flagged so the model reads it:

{{cite ohao/rpc/server.cpp "if (failed) { w.key("isError"); w.boolean(true); }"}}

A successful result is a single `text` block holding the compact result JSON. There is no `structuredContent`: MCP asks a server that sends structured content to send the same JSON as text as well, and that would double the tokens of every reply. An inline capture adds an `image` block next to the text block (see the capture unit).

`logging/setLevel` installs a log sink that turns engine records into `notifications/message {level, logger: channel, data}`. RFC 5424's eight level names map onto the engine's five (`notice` to warn; `critical`, `alert` and `emergency` to error). If the client asks for more detail than the logger currently captures, the capture threshold is lowered to match:

{{cite ohao/rpc/server.cpp "if (log::level() > *l) log::setLevel(*l);   // capture what the client asked for"}}

Without that call, no notifications are sent, so a client that never asks for logs pays nothing for them. Other `notifications/…` methods (`initialized`, `roots/list_changed`) are accepted and ignored. A notification that names a session method runs, and gets no reply.

## A queue with a ceiling

Messages that have been read but not yet run count against `ServerOptions::maxQueued`, 256 MiB. At the ceiling, the reader stops reading:

{{cite ohao/rpc/server.cpp "queueSpace.wait(lock, [&] { return queuedBytes < options.maxQueued || stopping; });"}}

The bytes then back up in the pipe, and a client that floods a busy engine blocks on its own writes instead of growing the server's memory. A single message is capped separately, at 64 MiB by default (`--max-message`). So the most memory a flood can cost is the queue ceiling plus one message. While the queue is full, `ping` waits too, because the reader is not reading. `stop()` sets `stopping`, which releases a reader waiting for space.

## Two ways to drive it

A headless server is `run()`: start the reader, then `poll()` with no timeout until the input ends, flushing after each batch of work:

{{cite ohao/rpc/server.cpp "while (poll(std::chrono::milliseconds(-1))) flush();"}}

The window host owns its own loop, so it calls the four parts itself. `start()` once. `poll(wait)` once per frame, which runs whatever is queued on the calling thread. `flush()` after the frame is presented. `stop()` when the window closes. With `holdReplies` set, the engine thread's replies are kept until `flush()`:

{{cite ohao/rpc/server.cpp "if (options.holdReplies) held.push_back(std::move(line));"}}

As a result, a reply to a request that changed the world goes out only after the frame showing that change has been presented. Progress notifications, log notifications and `ping` are sent straight to the transport, never held, since holding them would defeat their purpose. `stop()` also has to end a read that the client has left open; how it does that depends on the transport (see the transports unit).

:::why
Answering malformed lines from the engine thread costs a queue round trip for something the reader could reply to at once. The trade buys determinism: with every reply produced on one thread in arrival order, the serve goldens (`tests/serve/sessions` against `expected`) compare stdout byte for byte, including the error cases. Only `ping` and cancellation jump the queue, and both exist to say something about the queue itself.
:::

:::key
Two threads. The reader parses, queues, answers `ping` and handles cancellation. The engine thread runs one request at a time, in arrival order. The protocol's error codes are fixed (-32700, -32600, -32601, -32602), and domain errors carry the CLI's exit classes 1, 2 and 3. MCP is the same server: tools are named after methods, schemas come from params structs, and domain errors come back as `isError` results.
:::

## Contracts

- Only the engine thread calls the `Session`. A host that drives `poll()` must call it from the thread that owns the world.
- A cancelled request gets no response. Callers read the world's frame afterwards to learn where it stopped.
- End of input drops the queued requests unanswered: a client waits for its last reply before it closes.
- `holdReplies` holds replies only. A host that sets it must call `flush()`, or replies are never sent.
