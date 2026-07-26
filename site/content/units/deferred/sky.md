---
module: deferred
id: sky
title: Sky pass
---

## What

Sky pass composites HDR environment/atmosphere where GBuffer depth is sky.

## How

sky_pass + sky.frag after lighting.

## Why

Separate sky avoids lighting the background as geometry.

## Contracts

- Sources of truth: ohao/render/deferred/sky_pass.hpp
- Sources of truth: shaders/postprocess/sky.frag

## Notes

Composites HDR environment/atmosphere where GBuffer depth is sky.

## Notes

Source map:
- `ohao/render/deferred/sky_pass.hpp`
- `shaders/postprocess/sky.frag`
