---
module: deferred
id: csm
title: CSM shadows
---

## What

Cascaded shadow maps into a depth array; splits from the camera frustum. Uses unjittered camera when TAA is active for stable cascades.

## How

csm_pass records depth from light's view per cascade; shadow_csm.glsl samples with PCF.

## Why

Single shadow map cannot cover large outdoor views without swimming or acne.

## Contracts

- Sources of truth: ohao/render/deferred/csm_pass.hpp
- Sources of truth: shaders/shadow/shadow_csm.vert
- Sources of truth: shaders/includes/shadow/shadow_csm.glsl

## Notes

Cascaded shadow maps into depth array; splits from camera frustum.

Unjittered camera for stable cascades when TAA is on.

## Notes

Source map:
- `ohao/render/deferred/csm_pass.hpp`
- `shaders/shadow/shadow_csm.vert`
- `shaders/includes/shadow/shadow_csm.glsl`
