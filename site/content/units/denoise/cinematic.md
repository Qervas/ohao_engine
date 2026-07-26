---
module: denoise
id: cinematic
title: Cinematic RT post
---

## What

Cinematic RT post: bloom extract/blur, DoF, composite after denoise.

## How

cinematic_*.comp chain after NRD stability.

## Why

Bloom/DoF after denoise so temporal filters are not fighting post effects.

## Contracts

- Sources of truth: shaders/rt/cinematic_bloom_extract.comp
- Sources of truth: shaders/rt/cinematic_bloom_blur.comp
- Sources of truth: shaders/rt/cinematic_dof.comp
- Sources of truth: shaders/rt/cinematic_composite.comp

## Notes

Runs after denoise so bloom/DoF do not fight temporal filters.

Extract → blur mips → composite with optional DoF CoC.

## Notes

Source map:
- `shaders/rt/cinematic_bloom_extract.comp`
- `shaders/rt/cinematic_bloom_blur.comp`
- `shaders/rt/cinematic_dof.comp`
- `shaders/rt/cinematic_composite.comp`
