---
module: shaders
id: includes-fx
title: Atmosphere & water includes
---

## What

Gerstner water and cloud density procedural includes.

## How

Optional; not on cornell golden path.

## Why

Keep FX math out of lighting shaders until needed.

## Contracts

- Sources of truth: shaders/includes/water/gerstner.glsl
- Sources of truth: shaders/includes/cloud/cloud_density.glsl

## Notes

Procedural FX includes — used by sky/atmosphere experiments and water materials.

Not required on the cornell_box golden path; optional feature surface.

## Notes

Source map:
- `shaders/includes/water/gerstner.glsl`
- `shaders/includes/cloud/cloud_density.glsl`
