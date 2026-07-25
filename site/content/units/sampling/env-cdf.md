---
module: sampling
id: env-cdf
title: Environment CDF
---

## What

Environment map CDF for importance-sampled sky lighting.

## How

CPU builds marginal + conditional CDFs from HDR luminance (env_cdf.cpp).

GPU sampleEnvMap / pdfEnvMap in env_sampling.glsl.

## Why

Uniform env sampling wastes paths on dark texels.

## Contracts

- Sources of truth: ohao/render/rt/env_cdf.cpp
- Sources of truth: ohao/render/rt/env_cdf.hpp
- Sources of truth: shaders/includes/rt/env_sampling.glsl

## Notes

CPU builds marginal + conditional CDFs from HDR luminance.

GPU sampleEnvMap / pdfEnvMap for importance-sampled sky lighting in miss/NEE.

## Notes

Source map:
- `ohao/render/rt/env_cdf.cpp`
- `ohao/render/rt/env_cdf.hpp`
- `shaders/includes/rt/env_sampling.glsl`
