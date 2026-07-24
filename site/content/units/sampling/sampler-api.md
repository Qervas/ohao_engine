---
module: sampling
id: sampler-api
title: Sampler API
---

## What

Unified next1D/next2D sampler API; backend selected by profile specialization.

## How

sampler_api.glsl dispatches; sampler_types.hpp mirrors host enum.

## Why

Integrator code should not branch on RNG brand at every sample site.

## Contracts

- Sources of truth: shaders/includes/rt/sampler_api.glsl
- Sources of truth: ohao/render/rt/sampler_types.hpp

## Notes

Unified next1D/next2D API; backend selected by profile (PCG vs Sobol).

sampler_types.hpp mirrors host enum for specialization constants.

## Notes

Source map:
- `shaders/includes/rt/sampler_api.glsl`
- `ohao/render/rt/sampler_types.hpp`
