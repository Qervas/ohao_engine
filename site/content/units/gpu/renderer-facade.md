---
module: gpu
id: renderer-facade
title: VulkanRenderer facade
---

## What

VulkanRenderer is the public facade: initialize, setScene, setRenderMode, ensureRTRenderer, setDenoiseMode, render, getPixels/present.

RenderMode: Forward | Deferred | RTRealtime | RTOffline with isRTRenderMode helpers.

## How

renderer.hpp is the API; renderer.cpp + renderer_impl.hpp split implementation weight.

PathTracer is lazy so deferred-only sessions skip dual 4K RT allocations.

CameraUniformBuffer and SimpleVertex are GpuPod-asserted.


- VulkanRenderer(w,h).initialize()
- setScene → upload + BLAS/TLAS
- setRenderMode / ensureRTRenderer
- setDenoiseMode (None|OIDN|NRD|Atrous|DLSSRR)
- render() → getPixels / present

## Why

One facade for every example and golden test; modes share upload and AS.

## Contracts

- ensureRTRenderer before RT render
- setDenoiseMode after RT init for DLSS/NRD resource needs

## Notes

RenderMode: Forward | Deferred | RTRealtime | RTOffline. isRTRenderMode / isRasterRenderMode helpers.

PathTracer is lazy (ensureRTRenderer) so deferred-only sessions avoid dual 4K RT OOM.

renderer_impl.hpp holds private split of large renderer translation units.

CameraUniformBuffer and SimpleVertex are GpuPod-asserted shared layouts.

## Notes

Source map:
- `ohao/gpu/vulkan/renderer.hpp`
- `ohao/gpu/vulkan/renderer.cpp`
- `ohao/gpu/vulkan/renderer_impl.hpp`
- `ohao/gpu/gpu_module.hpp`
