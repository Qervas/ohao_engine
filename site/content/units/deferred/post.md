---
module: deferred
id: post
title: Post stack
---

## What

Post stack: bloom threshold/down/up, TAA resolve, tonemap (ACES family).

## How

PostProcessingPipeline owns order; bloom_pass and taa_pass share HDR/history targets.


- bloom threshold
- downsample
- upsample
- TAA
- tonemap

## Why

Tone map last so bloom sees HDR peaks; TAA before display reduces shimmer.

## Contracts

- Sources of truth: ohao/render/deferred/post_processing_pipeline.hpp
- Sources of truth: ohao/render/deferred/bloom_pass.hpp
- Sources of truth: ohao/render/deferred/taa_pass.hpp
- Sources of truth: shaders/postprocess/tonemapping.frag

## Notes

PostProcessingPipeline owns order; bloom and TAA are separate passes sharing history/HDR targets.

## Notes

Source map:
- `ohao/render/deferred/post_processing_pipeline.hpp`
- `ohao/render/deferred/bloom_pass.hpp`
- `ohao/render/deferred/taa_pass.hpp`
- `shaders/postprocess/tonemapping.frag`
- `shaders/postprocess/taa_resolve.frag`
