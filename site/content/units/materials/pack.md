---
module: materials
id: pack
title: GPU material pack
---

## What

GPU material pack: tight vec4 rows; texture indices bit-cast into floats for SSBO friendliness.

## How

layout_meta defines pack; upload writes rows in instance order.

Shaders unpack with floatBitsToUint style casts where needed.

## Why

std430-friendly packs beat pointer-chasing material structs on GPU.

## Contracts

- 0xFFFFFFFF = missing texture
- row order == TLAS instance order

## Notes

Material rows are tightly packed vec4s; texture indices bit-cast into floats for SSBO compatibility.

Bindless index 0xFFFFFFFF means 'no texture' — shaders must branch before sample.

layout_meta asserts sizes match GLSL std430 expectations.

## Notes

Source map:
- `ohao/gpu/layout_meta.hpp`
- `ohao/render/rt/gpu_light.hpp`
