---
module: deferred
id: ssao
title: SSAO
---

## What

Screen-space ambient occlusion: hemisphere samples in view space, blurred AO bound into lighting.

## How

ssao_pass + ssao.comp compute path.

## Why

Cheap contact darkening without full GI — complements hybrid RT and IBL.

## Contracts

- Sources of truth: ohao/render/deferred/ssao_pass.hpp
- Sources of truth: shaders/compute/ssao.comp

## Notes

Hemisphere samples in view space; blurred AO bound into lighting.

## Notes

Source map:
- `ohao/render/deferred/ssao_pass.hpp`
- `shaders/compute/ssao.comp`
