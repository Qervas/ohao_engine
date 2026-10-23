---
module: core
id: log
title: Logger
standard: v2
---

## One path for every engine message

Every message the engine writes goes through one macro, with a level, a channel and a
`std::format` string checked at compile time. The renderer, the GPU layer, physics,
asset loading, audio, the ECS and the runtime all use it; nothing under `ohao/` writes
to `std::cout`, `std::cerr` or `printf` any more, and a test keeps it that way:

{{cite tests/reflect/reflect_tests.cpp "static void sp4_t02_engine_prints_only_through_the_logger() {"}}

The reason is the agent server that follows it: `ohao serve` owns stdout for its
protocol, and a message an agent cannot query is a message it never sees. So the
logger keeps the last records in memory, where `log.read` can page them, and sends
them on to whatever sinks are attached.

## Below the threshold, one load and a compare

The macro checks the capture threshold before it touches its arguments. A record
below it costs a relaxed atomic load and a branch; its arguments are never evaluated,
so a `Debug` line in a hot loop is free in a run that does not want it:

{{cite ohao/core/log.hpp "return static_cast<uint8_t>(level) >= detail::g_threshold.load(std::memory_order_relaxed);"}}

Above it, the message is formatted into a per-thread buffer a few bytes larger than
the 1 KB limit, so the cut can land on a whole UTF-8 character:

{{cite ohao/core/log.hpp "thread_local char buf[cap];"}}

{{cite ohao/core/log.cpp "while (n > 0 && (static_cast<unsigned char>(msg[n]) & 0xC0) == 0x80) --n;"}}

`log_bench` measures both paths against their budgets: 0.20 ns below the threshold
(budget 5 ns) and 200 ns for an 80-character record into the ring (budget 300 ns).

## The ring

The ring holds the newest 4,096 records, each with a sequence number, the time since
start, the frame it was written in, its level, channel and text:

{{cite ohao/core/log.hpp "inline constexpr std::size_t kRingCapacity = 4096;"}}

A record identical to the one before it (same level, channel and text) does not take a
slot; it raises that record's repeat count, so a validation message fired every frame
costs one line, not four thousand:

{{cite ohao/core/log.cpp "if (last.level == level && last.channel == channel && last.msg == msg) {"}}

Reading is by sequence number: `read(since, minLevel, channel, limit)` returns what came
after `since`, and a reader that fell behind is told how many records it missed:

{{cite ohao/core/log.cpp "page.dropped = oldest - seq;"}}

The frame is stamped by the renderer as each deferred or RT frame starts, so a
validation message carries the frame it happened in:

{{cite ohao/gpu/vulkan/renderer.hpp "uint64_t m_framesStarted{0};"}}

## Sinks

A sink receives each new record at or above its own level, and each growth of the
newest record's repeat count, under the logger's lock and in record order. The default
sink writes `<L> <channel>: <message>` to stderr and folds a run of repeats into one
trailing line:

{{cite ohao/core/log.cpp "... repeated {} more times"}}

Vulkan validation messages are records on the `validation` channel, `Error` or `Warn`
by the layer's severity, as `<VUID> | <text>`; `check_validation.py` reads those
lines (and the repeat lines) back:

{{cite ohao/gpu/vulkan/device_setup.cpp "OHAO_LOG(Warn, validation,"}}

:::why
The logger's state is a leaked singleton, created on first use and never destroyed:

{{cite ohao/core/log.cpp "static State* s = new State;"}}

Engine objects log from destructors that run during static teardown; a logger that
was itself destroyed by then would be a use-after-free in exactly the path that
reports shutdown problems. The cost is one allocation the OS reclaims.
:::

:::key
One macro, ten channels, five levels. Below the threshold a call costs a load and a
compare; above it, a format into a thread-local buffer and a short critical section.
The ring is always on; sinks are optional; stdout is never written.
:::

## Contracts

- A sink must not log: it runs under the logger's lock, a `std::mutex`, and a sink
  that logs deadlocks on its first record.
- `seq` advances once per record, not per call: a repeat changes the newest record's
  `repeat` in place, so a reader that has already passed it does not see the new count.
- `OHAO_LOG_LEVEL` (`trace` … `error`, `off`) sets the threshold and the stderr sink's
  level at start-up; `setLevel` changes the threshold at run time.
