---
module: gpu
id: dispatch
title: Render dispatch
---

## What

Render dispatch branches on RenderMode: deferred graph vs PathTracer::render, with staging readback on the frame ring.

## How

Wait fence on ring slot → record → submit → advance frame.

getPixels memcpy only after wait — never overwrite in-flight staging.


- wait fence on ring slot
- memcpy staging → pixel buffer
- record cmd, submit, advance frame

## Why

MAX_FRAMES_IN_FLIGHT=3 hides latency; violating fence order races readback.

## Contracts

- Sources of truth: ohao/gpu/vulkan/render_dispatch.cpp
- Sources of truth: ohao/render/frame/frame_resources.hpp

## Notes

Branches on RenderMode: deferred graph vs PathTracer::render.

MAX_FRAMES_IN_FLIGHT=3; never overwrite in-flight staging.

## Notes

Source map:
- `ohao/gpu/vulkan/render_dispatch.cpp`
- `ohao/render/frame/frame_resources.hpp`
