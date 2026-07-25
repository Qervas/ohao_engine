---
module: denoise
id: types
title: Denoise modes & traits
---

## What

DenoiseMode enum + traits declaring which AOV guides each backend needs.

## How

denoise_types.hpp; CLI --denoise=dlssrr|nrd|oidn|atrous|none.

## Why

Traits prevent calling DLSS without hit-dist or NRD without packing.

## Contracts

- Sources of truth: ohao/render/rt/denoise/denoise_types.hpp
- Sources of truth: ohao/render/rt/rt_meta.hpp
- Sources of truth: ohao/render/rt/denoiser.hpp

## Notes

DenoiseMode: None | OIDN | NRD | Atrous | DLSSRR (and related).

Traits declare which AOV guides each backend needs (albedo, normal, hit-dist, motion).

CLI: --denoise=dlssrr | nrd | oidn | atrous | none.

## Notes

Source map:
- `ohao/render/rt/denoise/denoise_types.hpp`
- `ohao/render/rt/rt_meta.hpp`
- `ohao/render/rt/denoiser.hpp`
