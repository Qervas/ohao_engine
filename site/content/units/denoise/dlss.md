---
module: denoise
id: dlss
title: DLSS Ray Reconstruction
---

## What

NVIDIA DLSS Ray Reconstruction via NGX — guides include color, depth, motion, normal-roughness, specular hit-distance (binding 35).

## How

dlss_rr.hpp/cpp; needs LD_LIBRARY_PATH to DLSS .so; interactive --denoise=dlssrr.

dlss_tonemap.comp prepares display path.

## Why

Highest interactive quality on supported NVIDIA hardware.

## Contracts

- Sources of truth: ohao/render/rt/denoise/dlss_rr.hpp
- Sources of truth: ohao/render/rt/denoise/dlss_rr.cpp
- Sources of truth: shaders/compute/dlss_tonemap.comp

## Notes

NVIDIA DLSS Ray Reconstruction via NGX; needs LD_LIBRARY_PATH to DLSS .so.

Guides: color, depth, motion, normal-roughness, specular hit-distance (binding 35).

dlss_tonemap.comp prepares display path after RR.

interactive --denoise=dlssrr is the primary dogfood path.

## Notes

Source map:
- `ohao/render/rt/denoise/dlss_rr.hpp`
- `ohao/render/rt/denoise/dlss_rr.cpp`
- `shaders/compute/dlss_tonemap.comp`
