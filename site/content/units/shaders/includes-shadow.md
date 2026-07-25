---
module: shaders
id: includes-shadow
title: Shadow includes
---

## What

CSM/PCF sampling includes plus depth write shaders.

## How

shadow_csm.glsl, shadow_pcf.glsl, shadow_depth.*, shadow_csm.vert.

## Why

Sampling math must match cascade construction on the host.

## Contracts

- Sources of truth: shaders/includes/shadow/shadow_csm.glsl
- Sources of truth: shaders/includes/shadow/shadow_pcf.glsl
- Sources of truth: shaders/includes/shadow/shadow_types.glsl
- Sources of truth: shaders/shadow/shadow_depth.vert

## Notes

shadow_depth.* = single-map depth; shadow_csm.vert = cascade-aware depth write.

PCF/CSM sample helpers shared by deferred_lighting.frag.

## Notes

Source map:
- `shaders/includes/shadow/shadow_csm.glsl`
- `shaders/includes/shadow/shadow_pcf.glsl`
- `shaders/includes/shadow/shadow_types.glsl`
- `shaders/shadow/shadow_depth.vert`
- `shaders/shadow/shadow_depth.frag`
- `shaders/shadow/shadow_csm.vert`
