---
module: hybrid
id: shadow-technique
title: RT shadow technique
---

## What

RT shadow technique: visibility rays from GBuffer toward lights into a mask for deferred lighting.

## How

rt_shadow_technique + rt_shadow.rgen/rmiss/rahit.

## Why

Soft RT shadows without full path tracing cost.

## Contracts

- Sources of truth: ohao/render/rt/rt_shadow_technique.hpp
- Sources of truth: ohao/render/rt/rt_shadow_technique.cpp
- Sources of truth: shaders/rt/rt_shadow.rgen
- Sources of truth: shaders/rt/rt_shadow.rmiss

## Notes

Trace visibility rays from GBuffer world positions toward lights.

Output shadow mask sampled in deferred lighting — soft RT shadows without full PT.

## Notes

Source map:
- `ohao/render/rt/rt_shadow_technique.hpp`
- `ohao/render/rt/rt_shadow_technique.cpp`
- `shaders/rt/rt_shadow.rgen`
- `shaders/rt/rt_shadow.rmiss`
- `shaders/rt/rt_shadow.rahit`
