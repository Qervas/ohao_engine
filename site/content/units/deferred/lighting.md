---
module: deferred
id: lighting
title: Deferred lighting
---

## What

Fullscreen deferred lighting reconstructs world position from depth, evaluates GGX + IBL + CSM, multiplies SSAO, optionally samples RT shadow/GI textures.

## How

deferred_lighting.frag + deferred_lighting_pass.hpp; ibl.glsl for prefiltered env.

## Why

All shading in one pass over screen-sized buffers — light count scales better than forward.

## Contracts

- Sources of truth: ohao/render/deferred/deferred_lighting_pass.hpp
- Sources of truth: shaders/core/deferred_lighting.frag
- Sources of truth: shaders/includes/lighting/ibl.glsl

## Notes

Fullscreen pass reconstructs world pos from depth + inv VP.

Evaluates GGX + IBL + CSM; multiplies SSAO; optional RT shadow/GI textures.

## Notes

Source map:
- `ohao/render/deferred/deferred_lighting_pass.hpp`
- `shaders/core/deferred_lighting.frag`
- `shaders/includes/lighting/ibl.glsl`
