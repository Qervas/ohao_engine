---
module: shaders
id: rt-includes
title: RT shader includes
---

## What

RT-specific includes: pbr_unpack, rt_masks, nrd_frontend.

## How

Included by raygen/hit; nrd_frontend is the YCoCg pack source of truth.

## Why

Wrong NRD pack is a one-line bug with a magenta symptom — keep pack code centralized.

## Contracts

- Sources of truth: shaders/rt/includes/pbr_unpack.glsl
- Sources of truth: shaders/rt/includes/rt_masks.glsl
- Sources of truth: shaders/includes/rt/nrd_frontend.glsl
- Sources of truth: shaders/includes/pbr_unpack.glsl

## Notes

pbr_unpack: material row → albedo/metal/rough/F0 for hit and raygen.

rt_masks: visibility/anyhit mask bits.

nrd_frontend: YCoCg pack + norm hit-dist — wrong pack = magenta REBLUR.

## Notes

Source map:
- `shaders/rt/includes/pbr_unpack.glsl`
- `shaders/rt/includes/rt_masks.glsl`
- `shaders/includes/rt/nrd_frontend.glsl`
- `shaders/includes/pbr_unpack.glsl`
