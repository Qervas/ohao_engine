---
module: sampling
id: sobol
title: Sobol + Owen
---

## What

Sobol + Owen scramble for offline low-discrepancy sampling.

## How

Host tables + sampler_sobol.glsl; owen_scramble.cpp.

## Why

Lower discrepancy → faster offline convergence than white noise.

## Contracts

- Sources of truth: shaders/includes/rt/sampler_sobol.glsl
- Sources of truth: shaders/includes/rt/sampler_sobol_tables.glsl
- Sources of truth: ohao/render/rt/sobol_generator.cpp
- Sources of truth: ohao/render/rt/owen_scramble.cpp

## Notes

Sobol sequences + Owen scrambling for offline convergence.

Tables generated/loaded on host; GPU samples by dimension + pixel/sample index.

## Notes

Source map:
- `shaders/includes/rt/sampler_sobol.glsl`
- `shaders/includes/rt/sampler_sobol_tables.glsl`
- `ohao/render/rt/sobol_generator.cpp`
- `ohao/render/rt/owen_scramble.cpp`
