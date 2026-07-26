---
module: gpu
id: layout-meta
title: Layout contracts
---

## What

layout_meta.hpp is the single source of truth for GPU struct sizes/offsets shared with GLSL. Material and MaterialInstance are CPU authoring types that pack into SSBO rows.

## How

OHAO_ASSERT_GPU_LAYOUT fails the build if C++ and shader disagree.

MaterialInstance holds texture indices and factors before pack.

## Why

Silent layout drift is undebuggable; assert at compile time.

## Contracts

- Sources of truth: ohao/gpu/layout_meta.hpp
- Sources of truth: ohao/gpu/vulkan/material.hpp
- Sources of truth: ohao/gpu/vulkan/material.cpp
- Sources of truth: ohao/gpu/vulkan/material_instance.hpp

## Notes

layout_meta.hpp is the single source of truth for GPU struct sizes/offsets shared with GLSL.

Material + MaterialInstance are CPU-side authoring; packed rows feed bindless + PT material SSBO.

OHAO_ASSERT_GPU_LAYOUT fails the build if C++ and shader disagree — silent pink is worse.

## Notes

Source map:
- `ohao/gpu/layout_meta.hpp`
- `ohao/gpu/vulkan/material.hpp`
- `ohao/gpu/vulkan/material.cpp`
- `ohao/gpu/vulkan/material_instance.hpp`
- `ohao/gpu/vulkan/material_instance.cpp`
