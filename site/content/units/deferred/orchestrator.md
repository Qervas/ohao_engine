---
module: deferred
id: orchestrator
title: DeferredRenderer orchestrator
---

## What

DeferredRenderer owns the AAA raster stack: GBuffer, CSM, SSAO, lighting, sky, post, gizmo, optional RT shadow/GI inject.

## How

initialize(device, phys); setScene; setCameraData; set lights/geometry buffers.

render(cmd, frameIndex) runs passes; RenderGraph inserts barriers while passes own VkRenderPass objects.

onResize reallocates targets.


- CSM
- GBuffer
- SSAO
- Lighting (+ optional RT)
- Sky
- Bloom/TAA/Tonemap
- Gizmo

## Why

Orchestrator pattern keeps each pass small; hybrid RT plugs in without rewriting lighting.

## Contracts

- Sources of truth: ohao/render/deferred/deferred_renderer.hpp
- Sources of truth: ohao/render/deferred/deferred_renderer.cpp

## Notes

Owns GBuffer, CSM, lighting, SSAO, SSR/SSS, sky, post, gizmo, optional RT shadow/GI techniques.

render(cmd, frameIndex) is the per-frame entry; RenderGraph inserts barriers between passes.

setScene + geometry buffers come from VulkanRenderer after upload.

Hybrid: RTShadowTechnique / RTGITechnique inject visibility and bounce GI into lighting.

## Notes

Source map:
- `ohao/render/deferred/deferred_renderer.hpp`
- `ohao/render/deferred/deferred_renderer.cpp`
