---
module: sampling
id: mis
title: MIS heuristics
---

## What

Multiple importance sampling balance/power heuristics for NEE vs BRDF samples.

## How

mis.glsl; weights use pdf_light and pdf_bsdf in the same measure.

## Why

Either strategy alone is high variance on glossy + small lights.

## Contracts

- pdf units must match — solid angle vs area mix biases results

## Math

w_{\mathrm{bal}}(p_a,p_b)=\frac{p_a}{p_a+p_b}

## Notes

Balance heuristic default; power heuristic optional for NEE vs BRDF.

Weights use pdf_light and pdf_bsdf at the same sample — unit mismatch = bias.

## Notes

Source map:
- `shaders/includes/rt/mis.glsl`
