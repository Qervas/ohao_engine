---
module: shaders
id: postprocess-chain
title: Postprocess shader chain
---

## What

Fullscreen post chain: bloom, TAA, tonemap, sky, SSR, SSS shaders.

## How

fullscreen.vert feeds fragment posts; compute for SSR/SSS.


- threshold
- downsample
- upsample
- TAA
- tonemap

## Why

Pass ownership in deferred post pipeline; shaders stay dumb.

## Contracts

- Sources of truth: shaders/postprocess/fullscreen.vert
- Sources of truth: shaders/postprocess/bloom_threshold.frag
- Sources of truth: shaders/postprocess/bloom_downsample.frag
- Sources of truth: shaders/postprocess/bloom_upsample.frag

## Notes

fullscreen.vert feeds every post fragment pass.

Bloom: threshold → hierarchical down → up; TAA before final tonemap.

SSR/SSS are compute; optional quality knobs on DeferredRenderer.

## Notes

Source map:
- `shaders/postprocess/fullscreen.vert`
- `shaders/postprocess/bloom_threshold.frag`
- `shaders/postprocess/bloom_downsample.frag`
- `shaders/postprocess/bloom_upsample.frag`
- `shaders/postprocess/taa_resolve.frag`
- `shaders/postprocess/tonemapping.frag`
- `shaders/postprocess/sky.frag`
- `shaders/postprocess/ssr.comp`
- `shaders/postprocess/sss_blur.comp`
