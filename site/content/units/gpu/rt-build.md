---
module: gpu
id: rt-build
title: RT build (BLAS/TLAS)
---

## What

BLAS per mesh, TLAS over instances with world transforms. Instance order must match material table order for hit shaders.

## How

rt_build.cpp drives RTAccelerationStructure build/update.

Rebuild on topology change; refit/update when only transforms dirty.

## Why

Wrong instance→material mapping is the classic 'random materials on meshes' bug.

## Contracts

- instanceId indexes material rows
- world matrix from Transform only

## Notes

Per-mesh BLAS then TLAS instances with world transforms.

Invariant: TLAS instance order == material row order (hit shaders index by instanceId).

Rebuild on topology change; update transforms when only TRS dirty.

## Notes

Source map:
- `ohao/gpu/vulkan/rt_build.cpp`
- `ohao/render/rt/rt_acceleration_structure.hpp`
