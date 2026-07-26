---
module: denoise
id: atrous
title: À-trous / SVGF-style
---

## What

In-engine edge-aware à-trous / SVGF-style denoise with zero external dependency.

## How

atrous_denoise + denoise_atrous.comp / rt_svgf_*.comp.

## Why

Fallback when NRD/DLSS unavailable.

## Contracts

- Sources of truth: ohao/render/rt/denoise/atrous_denoise.hpp
- Sources of truth: shaders/compute/denoise_atrous.comp
- Sources of truth: shaders/rt/rt_atrous.comp
- Sources of truth: shaders/rt/rt_svgf_temporal.comp

## Notes

In-engine fallback when NRD/DLSS unavailable: edge-aware à-trous + optional SVGF temporal.

Does not match NRD quality but has zero external dependency.

## Notes

Source map:
- `ohao/render/rt/denoise/atrous_denoise.hpp`
- `shaders/compute/denoise_atrous.comp`
- `shaders/rt/rt_atrous.comp`
- `shaders/rt/rt_svgf_temporal.comp`
- `shaders/rt/rt_svgf_atrous.comp`
