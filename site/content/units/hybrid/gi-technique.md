---
module: hybrid
id: gi-technique
title: RT GI technique
---

## What

One-bounce RT GI inject into deferred lighting using GBuffer normals and material albedos.

## How

rt_gi_technique + rt_gi.rgen/rchit/rmiss; shares TLAS with shadows.

## Why

Middle ground between IBL-only and full PT.

## Contracts

- Sources of truth: ohao/render/rt/rt_gi_technique.hpp
- Sources of truth: ohao/render/rt/rt_gi_technique.cpp
- Sources of truth: shaders/rt/rt_gi.rgen
- Sources of truth: shaders/rt/rt_gi.rchit

## Notes

One-bounce GI from GBuffer normals; injects irradiance into deferred lighting.

Cheaper than full path tracer; shares TLAS with shadows.

## Notes

Source map:
- `ohao/render/rt/rt_gi_technique.hpp`
- `ohao/render/rt/rt_gi_technique.cpp`
- `shaders/rt/rt_gi.rgen`
- `shaders/rt/rt_gi.rchit`
- `shaders/rt/rt_gi.rmiss`
