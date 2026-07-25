---
module: path-tracer
id: host
title: Host lifecycle
---

## What

PathTracer host lifecycle: images (accum RGBA32F, output, AOVs), RT pipeline, SBT, descriptors, optional denoise backends.

Split TUs: pipeline, descriptors, images, render. ODR: NRD/DLSS members unconditional in header.

## How

init(device, phys, w, h) → allocate images → create pipeline/SBT → write descriptors

setMaterialAlbedos / lights / env CDF before first render

render(cmd, accel, view, proj, …); resetAccumulation on camera move

resize reallocates images and rewrites descriptors


- init(device, phys, w, h)
- setMaterialAlbedos / lights / env CDF
- render(cmd, accel, view, proj, …)
- resetAccumulation on camera move
- resize reallocates images + descriptor writes

## Why

Full-frame GI without a light bake; progressive samples until denoise or offline converge.

## Contracts

- Header layout identical with/without OHAO_NRD for ODR
- Do not construct dual 4K PathTracers casually

## Notes

PathTracer owns accum (RGBA32F), output (RGBA8), AOVs, SBT, and optional denoise backends.

Split TUs: pipeline create, descriptor update, image alloc/resize, render record.

rt_render_pipeline.hpp bridges VulkanRenderer mode dispatch into PathTracer::render.

ODR rule: NRD/DLSS members stay unconditional in the header so ohao_gpu_vulkan and ohao_renderer agree on layout.

## Notes

Source map:
- `ohao/render/rt/path_tracer.hpp`
- `ohao/render/rt/path_tracer.cpp`
- `ohao/render/rt/path_tracer_pipeline.cpp`
- `ohao/render/rt/path_tracer_descriptors.cpp`
- `ohao/render/rt/path_tracer_images.cpp`
- `ohao/render/rt/path_tracer_render.cpp`
- `ohao/render/rt/rt_module.hpp`
- `ohao/render/rt/rt_render_pipeline.hpp`
- `ohao/render/render_module.hpp`
