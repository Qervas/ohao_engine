---
module: shaders
id: compute
title: Compute shaders
---

## What

Compute suite: IBL bake, HiZ, light cull, skinning, composite, DoF, SSAO, denoise helpers.

## How

Dispatched from IBL processor, deferred passes, or PT post.

## Why

GPU prep work that is not full-screen fragment shading.

## Contracts

- Sources of truth: shaders/compute/brdf_lut.comp
- Sources of truth: shaders/compute/prefilter_envmap.comp
- Sources of truth: shaders/compute/equirect_to_cubemap.comp
- Sources of truth: shaders/compute/hiz_generate.comp

## Notes

IBL bake: equirect→cube, prefilter, BRDF LUT (once per env change).

HiZ + gpu_cull + light_culling feed deferred scalability.

skinning.comp updates skinned VB; dof_composite for cinematic RT path.

## Notes

Source map:
- `shaders/compute/brdf_lut.comp`
- `shaders/compute/prefilter_envmap.comp`
- `shaders/compute/equirect_to_cubemap.comp`
- `shaders/compute/hiz_generate.comp`
- `shaders/compute/light_culling.comp`
- `shaders/compute/gpu_cull.comp`
- `shaders/compute/skinning.comp`
- `shaders/compute/composite.comp`
- `shaders/compute/dof_composite.comp`
- `shaders/compute/ssao.comp`
- `shaders/compute/denoise_atrous.comp`
- `shaders/compute/dlss_tonemap.comp`
