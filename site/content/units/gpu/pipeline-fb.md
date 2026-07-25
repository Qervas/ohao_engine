---
module: gpu
id: pipeline-fb
title: Legacy pipeline & framebuffer
---

## What

Legacy forward pipeline and offscreen framebuffer helpers for simple demos and regression paths.

## How

pipeline.cpp creates forward graphics pipelines; framebuffer.cpp allocates color/depth for present/readback.

## Why

Forward remains for smoke tests; deferred is the production raster path.

## Contracts

- Sources of truth: ohao/gpu/vulkan/pipeline.cpp
- Sources of truth: ohao/gpu/vulkan/framebuffer.cpp

## Notes

Forward path remains for simple demos and regression; deferred is the production raster path.

Framebuffer helpers allocate offscreen color/depth used by present and readback.

## Notes

Source map:
- `ohao/gpu/vulkan/pipeline.cpp`
- `ohao/gpu/vulkan/framebuffer.cpp`
