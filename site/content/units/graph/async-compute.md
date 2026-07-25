---
module: graph
id: async-compute
title: Async compute queue
---

## What

Optional async compute queue path.

## How

async_compute_queue.hpp for overlapping work.

## Why

Headroom for culling/particles off the graphics queue.

## Contracts

- Sources of truth: ohao/render/async/async_compute_queue.hpp
- Sources of truth: ohao/render/async/async_compute_queue.cpp

## Notes

async_compute_queue.hpp: defines `class GpuAllocator`.

async_compute_queue.hpp: defines `enum class AsyncTaskStatus`.

async_compute_queue.hpp: defines `struct AsyncTaskHandle`.

## Notes

Source map:
- `ohao/render/async/async_compute_queue.hpp`
- `ohao/render/async/async_compute_queue.cpp`
