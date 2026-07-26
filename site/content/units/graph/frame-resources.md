---
module: graph
id: frame-resources
title: Frame resources ring
---

## What

Per-frame ring: fences, staging, UBOs with MAX_FRAMES_IN_FLIGHT=3.

## How

frame_resources.hpp/cpp; dispatch waits before reuse.

## Why

CPU/GPU overlap without resource hazard.

## Contracts

- Sources of truth: ohao/render/frame/frame_resources.hpp
- Sources of truth: ohao/render/frame/frame_resources.cpp

## Notes

frame_resources.hpp: defines `struct FrameResources`.

frame_resources.hpp: defines `struct FrameCameraUBO`.

frame_resources.hpp: defines `class FrameResourceManager`.

## Notes

Source map:
- `ohao/render/frame/frame_resources.hpp`
- `ohao/render/frame/frame_resources.cpp`
