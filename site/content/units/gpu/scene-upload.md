---
module: gpu
id: scene-upload
title: Scene upload
---

## What

Scene upload walks MeshComponents into VB/IB maps, packs material rows, dual-packs lights for deferred vs path tracer, and prepares env handles.

## How

scene_upload.cpp builds interleaved geometry and MeshBufferInfo map.

light_upload.cpp packs LightComponents into GPULight SSBO and deferred LightData.

No Vulkan types remain in scene/ — this boundary owns the conversion.

## Why

Upload is the only place scene becomes GPU state; keeping it explicit makes TLAS/material lockstep enforceable.

## Contracts

- Sources of truth: ohao/gpu/vulkan/scene_upload.cpp
- Sources of truth: ohao/gpu/vulkan/light_upload.cpp

## Notes

Walks MeshComponents → interleaved VB/IB + MeshBufferInfo map.

Material rows packed into SSBO; lights dual-pack for deferred vs PT.

No Vulkan types in scene/ — upload is the boundary.

## Notes

Source map:
- `ohao/gpu/vulkan/scene_upload.cpp`
- `ohao/gpu/vulkan/light_upload.cpp`
