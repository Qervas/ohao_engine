---
module: shaders
id: layout
title: Shader tree layout
---

## What

Shader tree layout: core, rt, postprocess, compute, includes, shadow, particles. SPIR-V via CMake/compile script.

## How

PathTracer searches multiple relative SPIR-V paths; include root is shaders/.

## Why

Predictable layout beats hunting random .spv copies.

## Contracts

- Sources of truth: shaders/CMakeLists.txt
- Sources of truth: shaders/compile_shaders.sh

## Notes

SPIR-V outputs to build/shaders or bin/shaders; PathTracer searches multiple relative paths.

Active sources only — shaders/_disabled is archival and not in the build graph.

Include path: GL_GOOGLE_include_directive with shaders/ as root.

## Notes

Source map:
- `shaders/CMakeLists.txt`
- `shaders/compile_shaders.sh`
