---
module: shaders
id: includes-common
title: Common includes
---

## What

Shared math, color, encoding, reconstruction, constants, noise.

## How

Used by both raster and RT; encoding holds octahedral pack conventions.

## Why

One luminance/normalize definition everywhere.

## Contracts

- Sources of truth: shaders/includes/common/math.glsl
- Sources of truth: shaders/includes/common/color.glsl
- Sources of truth: shaders/includes/common/encoding.glsl
- Sources of truth: shaders/includes/common/reconstruction.glsl

## Notes

Shared helpers for both raster and RT — keep one definition of luminance, safe normalize, etc.

encoding.glsl: octahedral / pack conventions used by GBuffer and AOVs.

noise.glsl: procedural hash for SSAO, particles, cloud density.

## Notes

Source map:
- `shaders/includes/common/math.glsl`
- `shaders/includes/common/color.glsl`
- `shaders/includes/common/encoding.glsl`
- `shaders/includes/common/reconstruction.glsl`
- `shaders/includes/common/constants.glsl`
- `shaders/includes/common/types.glsl`
- `shaders/includes/common/common.glsl`
- `shaders/includes/noise.glsl`
