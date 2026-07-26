---
module: gpu
id: bindless
title: Bindless textures
---

## What

BindlessTextureManager: variable-count sampled image array with UPDATE_AFTER_BIND so materials store integer indices instead of descriptor sets.

## How

Allocate large array binding; write descriptor for each uploaded texture.

Shaders sample via nonuniformEXT(index); 0xFFFFFFFF means unbound.

## Why

Per-material descriptor thrash kills multi-texture GLB load times and bind cost.

## Contracts

- Never sample index 0xFFFFFFFF
- Update-after-bind set flags must match layout

## Notes

Variable-count sampled image array with UPDATE_AFTER_BIND.

Materials store integer indices; shaders use nonuniformEXT.

Index 0xFFFFFFFF = unbound — never sample without a valid index.

## Notes

Source map:
- `ohao/gpu/vulkan/bindless_texture_manager.hpp`
- `ohao/gpu/vulkan/bindless_texture_manager.cpp`
