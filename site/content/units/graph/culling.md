---
module: graph
id: culling
title: Culling
---

## What

CPU/GPU cull helpers (HiZ, light cull compute).

## How

culling.hpp + gpu_cull.comp / hiz_generate.comp.

## Why

Scale deferred to denser scenes.

## Contracts

- Sources of truth: ohao/render/culling.hpp
- Sources of truth: shaders/compute/gpu_cull.comp
- Sources of truth: shaders/compute/hiz_generate.comp

## Notes

culling.hpp: defines `struct FrustumPlane`.

culling.hpp: defines `struct AABB`.

culling.hpp: defines `class Frustum`.

gpu_cull.comp: defines `struct MeshDescriptor`.

GPU Frustum Culling Culls objects against view frustum and outputs visible draw commands Mesh descriptor (bounding volume + draw info)

Hi-Z (Hierarchical-Z) Buffer Generation Generates depth pyramid for efficient ray marching For mip 0: sample from depth buffer Write to current mip level

## Notes

Source map:
- `ohao/render/culling.hpp`
- `shaders/compute/gpu_cull.comp`
- `shaders/compute/hiz_generate.comp`
