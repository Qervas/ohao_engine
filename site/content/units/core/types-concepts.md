---
module: core
id: types-concepts
title: Types & concepts
---

## What

Shared C++20 types and concepts used across modules. GpuPod constrains GPU-shared structs to trivially copyable layouts so CPU packs match GLSL std430/std140.

## How

common_types.hpp holds enums/aliases (RenderMode-adjacent flags, mesh ids).

concepts.hpp defines GpuPod and related requires-clauses used by static_assert on camera UBO, SimpleVertex, GPULight.

core.hpp is the umbrella include for consumers that only need core.

## Why

Without layout discipline, deferred and path tracer silently disagree on material rows — pink metals and NaN lights.

## Contracts

- GpuPod types are trivially copyable
- No Vulkan types in core

## Notes

GpuPod constrains GPU-shared structs to trivially copyable layouts.

common_types holds shared enums/aliases used by scene and renderer.

## Notes

Source map:
- `ohao/core/common_types.hpp`
- `ohao/core/concepts.hpp`
- `ohao/core/core.hpp`
