---
module: sampling
id: pcg
title: PCG realtime
---

## What

PCG hash RNG for interactive path tracing.

## How

sampler_pcg.glsl — cheap per-pixel streams.

## Why

Interactive budgets cannot afford full Sobol table traffic every frame.

## Contracts

- Sources of truth: shaders/includes/rt/sampler_pcg.glsl

## Notes

Cheap hash RNG for interactive path tracing; quality traded for speed vs Sobol.

## Notes

Source map:
- `shaders/includes/rt/sampler_pcg.glsl`
