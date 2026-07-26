---
module: denoise
id: oidn
title: OIDN
---

## What

Intel Open Image Denoise for offline/reference quality.

## How

oidn_denoise after enough spp; guided by albedo/normal when available.

## Why

Best quality when latency is irrelevant.

## Contracts

- Sources of truth: ohao/render/rt/denoise/oidn_denoise.hpp
- Sources of truth: ohao/render/rt/denoise/oidn_denoise.cpp

## Notes

Intel Open Image Denoise for offline/reference — quality over latency.

Typically runs after enough spp accumulate; guided by albedo/normal when available.

## Notes

Source map:
- `ohao/render/rt/denoise/oidn_denoise.hpp`
- `ohao/render/rt/denoise/oidn_denoise.cpp`
