---
module: shaders
id: includes-lighting
title: Lighting includes
---

## What

IBL, attenuation, light types, phase, blinn_phong helpers.

## How

ibl.glsl samples prefiltered specular + irradiance + BRDF LUT.

## Why

Deferred and hybrid share light math with CPU packs.

## Contracts

- Sources of truth: shaders/includes/lighting/ibl.glsl
- Sources of truth: shaders/includes/lighting/light_attenuation.glsl
- Sources of truth: shaders/includes/lighting/light_types.glsl
- Sources of truth: shaders/includes/lighting/phase.glsl

## Notes

ibl.glsl samples prefiltered specular + irradiance + BRDF LUT for deferred and hybrid.

light_types.glsl mirrors CPU LightData / GPULight packing conventions.

phase.glsl for volume/cloud scattering lobes.

## Notes

Source map:
- `shaders/includes/lighting/ibl.glsl`
- `shaders/includes/lighting/light_attenuation.glsl`
- `shaders/includes/lighting/light_types.glsl`
- `shaders/includes/lighting/phase.glsl`
- `shaders/includes/lighting/blinn_phong.glsl`
