---
module: core
id: console-widget
title: Console widget
standard: v2
---

## The widget that is not a widget

Nothing here draws — no ring buffer, no scrollback, no UI. The name is a fossil:
OHAO shipped a full ImGui editor early on and deliberately deleted it, and the
class kept the label.

{{cite docs/architecture/README.md@223ff7f "ImGui-based editor UI"}}

What survives is a process-wide, mutex-guarded facade over `std::cout` and
`std::cerr` plus one unused redirect hook, reached through a Meyers singleton:

{{cite ohao/core/console_widget.hpp "static ConsoleWidget instance;"}}

`get()` is defined in the class body, so it is implicitly inline and the ODR
collapses the function-local static to one object per *program* — not one per
build. `ohao_core` is an archive linked separately into each example and test
executable, so each process gets its own logger, mutex and callback slot.

## From macro to ostream

Call sites never touch the class. A macro fetches the singleton and passes the
call site's `std::source_location` explicitly:

{{cite ohao/core/console_widget.hpp "#define OHAO_LOG(msg)"}}

`log`, `logWarning`, `logError` and `logDebug` are one-line forwarders into a
private `emit`, which takes the lock and writes tag, message and newline in one
critical section, so two threads cannot shred each other's lines:

{{cite ohao/core/console_widget.cpp "os << '[' << logLevelName(level)"}}

It serialises facade calls against each other only; the hundreds of raw
`std::cout`/`std::cerr` writes elsewhere under `ohao/` can still cut a formatted
line in half.

Level routing is one ternary — only `Error` reaches stderr:

{{cite ohao/core/console_widget.cpp "return level == LogLevel::Error ? std::cerr : std::cout;"}}

The taxonomy leaks the other way too: seven of the eleven facade calls in
`profile_manager.cpp` spell the severity into the message and hand it to the
Info-level macro,

{{cite ohao/physics/world/profile_manager.cpp "+ key +"}}

shipping `[INFO] Error: Profile '…' already exists` on stdout.

## Wired but never exercised

All 69 call sites materialise a `source_location` and thread it through the
forwarder into `emit`, which discards it: the flag that would print it
initialises off,

{{cite ohao/core/console_widget.hpp "bool m_includeLocation{false};"}}

contradicting the declaration comment one screen above.

{{cite ohao/core/console_widget.hpp "When true (default), prefix messages with file:line"}}

Trust the initialiser — `setIncludeLocation` has zero callers. Turning it on is
one line, with one exception: the templated `operator<<` calls `log(ss.str())` on
the *default* argument, and a default `source_location::current()` is evaluated at
that call, inside the header, so stream-style logging would report
`console_widget.hpp`, never the caller.

The other dormant path is a `std::function<void(LogLevel, std::string_view)>`
redirect. When set, `emit` hands the message over and returns before any stream
write:

{{cite ohao/core/console_widget.cpp "m_logCallback(level, message);"}}

No tag, no newline — the sink owns formatting. `loc` is not in the callback type,
so a sink can never show file:line whatever `setIncludeLocation` says; worse, the
callback runs while `emit` still holds `m_mutex`, a non-recursive `std::mutex`, so
a sink that logs, or that reinstalls itself via `setLogCallback`, deadlocks on its
first message. `setLogCallback`, `clearLogCallback`, `logAt` and `operator<<` have
no callers anywhere: the reachable surface of this class is the five macros.

## Four logging conventions in one engine

Ten `.cpp` files log through the facade, 69 call sites, all in `physics/` and
`scene/`. The renderer does not participate: under `ohao/`, 56 files write
`std::cout` directly (346 lines) and 51 write `std::cerr` directly (287 lines),
while `audio/` uses `fprintf`. Those raw writes do separate severity — Vulkan
device setup puts its failure on stderr and its success on stdout five lines
later, just without the facade:

{{cite ohao/gpu/vulkan/device_setup.cpp "Failed to create logical device with RT extensions"}}

So a callback installed here would capture `physics/`'s facade traffic, under a
third of `scene/`'s log output (30 facade calls against 78 raw lines), and nothing
from Vulkan or the path tracer. The header also includes `core/concepts.hpp` for a
`to_underlying` it never calls, costing every logging TU a
`<concepts>`/`<ranges>`/`<span>` pull-in.

:::why
The whole logging strategy is this file: no third-party logger, no sink registry,
no compile-time level filter — and `ohao_core` declares no link dependencies at
all, so physics and scene need nothing beyond libstdc++ to log:

{{cite ohao/core/CMakeLists.txt "add_library(ohao_core STATIC"}}

spdlog or fmt would buy level filtering and compiled-out debug lines for a link
dependency in the engine's lowest layer, inherited by every target. The missing
filter costs nothing yet: `OHAO_LOG_DEBUG` is unconditional in release as in
debug, but the tree holds exactly one use of it — inside
`ForceDebugger::logBodyForceBreakdown`, which nothing calls.

{{cite ohao/physics/debug/force_debugger.cpp "OHAO_LOG_DEBUG(log.str());"}}
:::

:::key
Everything here that looks like infrastructure — location capture, the callback
redirect, the stream operator — is wired but never exercised. Shipping behaviour
is `[LEVEL] message` on stdout, stderr only for `Error`, with the severity
sometimes spelled into the message instead of the level.
:::

## Contracts

- `emit` holds `m_mutex` across the callback invocation. A callback that logs, or
  that calls `setLogCallback`, deadlocks — `std::mutex` is not recursive.
- `<iostream>` is included by the header, not the implementation, which declares
  only `<ostream>`. `std::cout`/`std::cerr` reach `emit` transitively, so tidying
  the header's include list breaks the build.
- Only `LogLevel::Error` goes to stderr; `Warning` and `Debug` stay on stdout. A
  `2>errors.log` triage does not lose them, it just never shows them.
