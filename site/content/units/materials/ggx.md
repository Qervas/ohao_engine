---
module: materials
id: ggx
title: GGX implementation
---

## What

GGX/Trowbridge-Reitz microfacet BRDF with Schlick Fresnel and Smith correlated geometry — shared by deferred lighting and path-tracer lobes.

## How

brdf_ggx.glsl: D, F, G terms; brdf_common helpers; ggx_aniso for anisotropic roughness.

material_sampling uses VNDF/cosine for importance sampling in PT.

## Why

Two BRDFs = two lookdevs. One GGX is a product decision.

## Contracts

- Sources of truth: shaders/includes/brdf/brdf_ggx.glsl
- Sources of truth: shaders/includes/brdf/brdf_common.glsl
- Sources of truth: shaders/includes/material/ggx_aniso.glsl

## Math

f_r = \frac{D F G}{4(n\cdot w_i)(n\cdot w_o)}

## Notes

Trowbridge-Reitz D, Schlick F, Smith G (correlated form in brdf_ggx.glsl).

Same GGX used by deferred lighting and path-tracer lobes — one visual language.

ggx_aniso.glsl extends to anisotropic roughness for brushed metals.

## Notes

Source map:
- `shaders/includes/brdf/brdf_ggx.glsl`
- `shaders/includes/brdf/brdf_common.glsl`
- `shaders/includes/material/ggx_aniso.glsl`
