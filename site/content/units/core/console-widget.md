---
module: core
id: console-widget
title: Console widget
---

## What

Optional in-engine console / log surface for debug tooling. Not on the hot render path.

## How

ConsoleWidget buffers log lines and presents a simple UI surface for tools/examples.

Stays in core so gpu/ remains free of UI dependencies.

## Why

Debuggability without dragging ImGui or similar into every library target.

## Contracts

- Sources of truth: ohao/core/console_widget.hpp
- Sources of truth: ohao/core/console_widget.cpp

## Notes

Optional UI-facing log sink used by tools and examples; not on the hot render path.

Keeps console I/O out of gpu/ so core stays Vulkan-free.

## Notes

Source map:
- `ohao/core/console_widget.hpp`
- `ohao/core/console_widget.cpp`
