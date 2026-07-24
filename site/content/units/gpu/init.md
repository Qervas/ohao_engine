---
module: gpu
id: init
title: Device init
---

## What

Device init chain: instance → physical device → logical device with RT/descriptor indexing/BDA features.

## How

Vulkan 1.3 instance; prefer discrete GPU + graphics queue.

Enable accelerationStructure, rayTracingPipeline, bufferDeviceAddress, descriptorIndexing.

Optional DLSS device extensions when built with NGX.

Command pool, sync objects, frame resources allocated after device.


- Vulkan 1.3 instance
- Prefer discrete GPU + graphics queue
- Enable AS + RT pipeline + descriptor indexing + BDA
- Optional DLSS device exts
- Command pool, sync, frame resources

## Why

Missing RT features fail at pipeline create, not first draw — early, loud, fixable.

## Contracts

- Sources of truth: ohao/gpu/vulkan/device_setup.cpp
- Sources of truth: ohao/gpu/vulkan/renderer.cpp

## Notes

Feature chain must enable accelerationStructure, rayTracingPipeline, bufferDeviceAddress, descriptorIndexing.

Without those, RT path fails at pipeline create — not at first draw.

## Notes

Source map:
- `ohao/gpu/vulkan/device_setup.cpp`
- `ohao/gpu/vulkan/renderer.cpp`
