---
module: materials
id: lights
title: Lights dual packing
---

## What

Dual light packing: GPULight 64-byte SSBO for path tracer (sphere/dir/spot/area) vs compact deferred LightData UBO.

## How

gpu_light.hpp defines GPULight; light_upload.cpp packs from LightComponents.

Sphere lights store radius in dirAndParam.w; NEE must use distance for shadow Tmax, not radius.

## Why

PT needs analytic area sampling; deferred needs a tight multi-light loop — one component model, two packs.

## Contracts

- NEE shadow ray uses lightDist for Tmax, not light radius
- lightCount heads the SSBO

## Notes

Path tracer: GPULight 64-byte SSBO (position, type, color, dir, spot/area extras).

Deferred: compact LightData UBO for the forward-compatible light loop.

light_upload.cpp packs scene LightComponents into both layouts as needed.

Sphere lights use radius in dirAndParam.w; NEE must use distance for shadow Tmax, not radius.

## Notes

Source map:
- `ohao/render/rt/gpu_light.hpp`
- `ohao/gpu/vulkan/light_upload.cpp`
- `shaders/includes/lighting/light_types.glsl`
