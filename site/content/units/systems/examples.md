---
module: systems
id: examples
title: Examples map
---

## What

Example map: cornell_box, model_viewer, env_demo, interactive, turntable share example_cli.hpp flags.

## How

interactive is full dogfood (WASD, mode switch, --denoise=dlssrr).

## Why

Thin drivers over one API — golden and demo paths stay aligned.

## Contracts

- Sources of truth: examples/cornell_box.cpp
- Sources of truth: examples/model_viewer.cpp
- Sources of truth: examples/env_demo.cpp
- Sources of truth: examples/interactive.cpp

## Notes

example_cli.hpp shared flags: model, env, mode, denoise, spp.

interactive: WASD camera + mode switch + --denoise=dlssrr.

cornell_box / turntable feed golden and showcase pipelines.

Research-only CLIs under examples/ are out of the public monograph scope.

## Notes

Source map:
- `examples/cornell_box.cpp`
- `examples/model_viewer.cpp`
- `examples/env_demo.cpp`
- `examples/interactive.cpp`
- `examples/turntable.cpp`
- `examples/example_cli.hpp`
