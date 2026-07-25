---
module: architecture
id: invariants
title: Cross-module invariants
---

## What

Cross-module invariants are product rules that break pixels silently if violated — not style preferences.

They couple scene upload, hit shaders, denoisers, and the frame ring into one coherent system.

## How

Documented in STATUS and chapter contracts; enforced by layout asserts, golden images, and careful descriptor writes.

Upload path packs materials in the same order BLAS instances are built.

NRD packing happens in raygen via nrd_frontend.glsl before REBLUR sees the image.

## Why

Silent pink, black frames, and OOM are worse than a loud assert. Invariants make failure modes diagnosable.

## Contracts

- TLAS instance order == material row order
- Bindless 0xFFFFFFFF = no texture
- Frame ring: wait fence before staging readback
- NRD: YCoCg + norm hit-dist (not linear RGB)
- Lazy PathTracer profiles (no dual 4K OOM)
- Same PBR language for deferred and PT

## Notes

These are product rules, not style preferences. Documented in STATUS and chapter contracts.

## Notes

Source map:
