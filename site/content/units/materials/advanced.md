---
module: materials
id: advanced
title: Advanced material includes
---

## What

Advanced material includes: multi-lobe helpers, sampling routines, material type constants used by hit and raygen.

## How

material_sampling.glsl importance samples GGX VNDF / cosine hemisphere.

material_types.glsl shares enum-like constants.

advanced_brdf.glsl extends beyond simple metal-rough.

## Why

Keep lobe math out of the giant raygen file so offline/realtime variants share code.

## Contracts

- Sources of truth: shaders/includes/material/advanced_brdf.glsl
- Sources of truth: shaders/includes/material/material_sampling.glsl
- Sources of truth: shaders/includes/material/material_types.glsl

## Notes

material_sampling: importance sample GGX VNDF / cosine hemisphere for PT.

material_types: enum-like constants shared across hit and raygen.

advanced_brdf: multi-lobe helpers beyond simple metal-rough.

## Notes

Source map:
- `shaders/includes/material/advanced_brdf.glsl`
- `shaders/includes/material/material_sampling.glsl`
- `shaders/includes/material/material_types.glsl`
