---
module: gpu
id: buffers-alloc
title: Buffers & allocator
---

## What

GpuAllocator and buffer setup helpers for device-local buffers, staging, and images used by the graph and path tracer.

## How

Staging for CPU→GPU mesh/material uploads; ring-friendly lifetimes with frame resources.

vk_utils.hpp: create helpers, debug names, barrier one-liners.

## Why

Central allocation avoids ad-hoc vkAllocateMemory copies and leaks on resize.

## Contracts

- Sources of truth: ohao/gpu/vulkan/gpu_allocator.hpp
- Sources of truth: ohao/gpu/vulkan/gpu_allocator.cpp
- Sources of truth: ohao/gpu/vulkan/buffer_setup.cpp
- Sources of truth: ohao/gpu/vulkan/vk_utils.hpp

## Notes

GpuAllocator wraps device memory for buffers/images used by graph and PT.

Staging buffers for CPU→GPU mesh/material uploads; ring-friendly lifetimes.

vk_utils: create helpers, debug names, barrier one-liners.

## Notes

Source map:
- `ohao/gpu/vulkan/gpu_allocator.hpp`
- `ohao/gpu/vulkan/gpu_allocator.cpp`
- `ohao/gpu/vulkan/buffer_setup.cpp`
- `ohao/gpu/vulkan/vk_utils.hpp`
