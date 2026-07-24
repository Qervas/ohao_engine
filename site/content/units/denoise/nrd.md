---
module: denoise
id: nrd
title: NRD REBLUR + compose
---

## What

NRD REBLUR realtime denoise: pack radiance as YCoCg + normalized hit-distance, dispatch, compose unpack, optional cinematic post.

## How

nrd_frontend.glsl pack in raygen; NrdDenoiser dispatch; nrd_compose.comp unpack; NrdCinematicPost optional.


- pack AOVs
- NRD dispatch
- compose unpack
- optional cinematic

## Why

Temporal denoise reuses AOVs already required for hybrid/PT. Wrong pack = magenta classic bug.

## Contracts

- IN_DIFF/SPEC are YCoCg+hitDist not linear RGB
- Compose must unpack before display

## Notes

REBLUR expects IN_DIFF/SPEC as YCoCg + normalized hit-distance — not linear RGB.

nrd_frontend.glsl: nrdPackRadianceHitDist / nrdYCoCgToLinear.

Compose unpacks after REBLUR; wrong pack = magenta or washed output (classic bug).

NrdCinematicPost optional bloom/DoF after stable denoised HDR.

## Notes

Source map:
- `ohao/render/rt/denoise/nrd_denoise.hpp`
- `ohao/render/rt/denoise/nrd_compose.hpp`
- `ohao/render/rt/denoise/nrd_cinematic.hpp`
- `shaders/includes/rt/nrd_frontend.glsl`
- `shaders/rt/nrd_compose.comp`
