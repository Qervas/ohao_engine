---
module: shaders
id: core-gbuffer-lighting
title: Core GBuffer & lighting shaders
---

## What

Core raster pair: gbuffer.vert/frag MRT contract and deferred_lighting.frag.

## How

Bindless nonuniformEXT; lighting reconstructs position from depth.

## Why

These two shaders are the deferred product surface.

## Contracts

- Sources of truth: shaders/core/gbuffer.vert
- Sources of truth: shaders/core/gbuffer.frag
- Sources of truth: shaders/core/deferred_lighting.frag

## Notes

GBuffer MRT: albedo, normal, material, velocity (from current/prev clip pos), depth.

Bindless texture indices via nonuniformEXT; 0xFFFFFFFF = missing map.

deferred_lighting.frag: light loop + IBL + CSM + optional RT shadow/GI textures.

## Notes

Source map:
- `shaders/core/gbuffer.vert`
- `shaders/core/gbuffer.frag`
- `shaders/core/deferred_lighting.frag`
