---
module: architecture
id: request-path
title: Request path
---

## What

The request path is the single mental model for every OHAO binary: a thin driver builds a Scene, hands it to VulkanRenderer, chooses a RenderMode, and either presents a swapchain or reads pixels.

Examples (interactive, cornell_box, turntable, model_viewer) share this path; they differ only in scene construction and CLI flags.

## How

Parse CLI (model path, HDR env, --mode, --denoise, spp).

Construct Scene; loaders fill Mesh/Material/Light components without touching Vulkan.

VulkanRenderer(w,h).initialize() creates instance, device, queues, frame ring, bindless pool.

setScene uploads geometry and materials, then buildBLASTLAS when RT is needed.

setRenderMode selects Forward, Deferred, RTRealtime, or RTOffline; ensureRTRenderer lazily constructs PathTracer.

render() records and submits; getPixels copies staging after fence wait for offline/goldens.


- Parse CLI (model, env, mode, denoise)
- Build Scene + load assets into components
- VulkanRenderer(w,h).initialize() cold start
- setScene → upload + BLAS/TLAS
- setRenderMode / ensureRTRenderer as needed
- render() loop or single shot; getPixels / present

## Why

One API surface means tests, demos, and tools never fork renderer semantics. Lazy RT avoids dual 4K path-tracer OOM when you only needed deferred.

## Contracts

- Scene must not include vulkan.h
- initialize before setScene
- wait fence before staging readback
- TLAS instance order matches material rows after upload

## Notes

The engine is a library: no editor host required.

Examples are thin drivers over the same API surface.

## Notes

Source map:
- `examples/interactive.cpp`
- `examples/cornell_box.cpp`
- `ohao/gpu/vulkan/renderer.cpp`
