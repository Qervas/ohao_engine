---
module: path-tracer
id: as
title: Acceleration structures
---

## What

RTAccelerationStructure: BLAS/TLAS build, rebuild, update.

## How

rt_acceleration_structure.cpp used by both hybrid and full PT.

## Why

One AS owner avoids divergent instance layouts.

## Contracts

- Sources of truth: ohao/render/rt/rt_acceleration_structure.hpp
- Sources of truth: ohao/render/rt/rt_acceleration_structure.cpp

## Notes

rt_acceleration_structure.hpp: defines `struct BlasEntry`.

rt_acceleration_structure.hpp: defines `struct RTInstance`.

rt_acceleration_structure.hpp: defines `class RTAccelerationStructure`.

RT Acceleration Structure Manager for OHAO Engine Manages Bottom-Level (BLAS) and Top-Level (TLAS) acceleration structures for Vulkan ray tracing.

NVIDIA-only (VK_KHR_acceleration_structure).

Usage: RTAccelerationStructure accel; accel.init(device, physicalDevice, graphicsQueue, commandPool); uint32_t blasHandle = accel.createBLAS(vertexBuffer, indexBuffer, ...); accel.addInstance(blasHandle, transform); accel.buildTLAS(cmd); Each BLAS is a single geometry (mesh).

TLAS holds instances referencing BLASes.

## Notes

Source map:
- `ohao/render/rt/rt_acceleration_structure.hpp`
- `ohao/render/rt/rt_acceleration_structure.cpp`
