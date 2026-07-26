---
module: deferred
id: gbuffer
title: GBuffer pass
---

## What

GBuffer pass rasterizes scene into MRT: albedo, normals, material params, motion vectors, depth — fuel for deferred lighting and denoisers.

## How

gbuffer.vert: world pos, normal, tangent, dual UV, current+prev clip for velocity.

gbuffer.frag: bindless textures, material packing, nonuniformEXT indices.

## Why

Decouple geometry bandwidth from light count; motion vectors unlock TAA and DLSS.

## Contracts

- Sources of truth: ohao/render/deferred/gbuffer_pass.hpp
- Sources of truth: shaders/core/gbuffer.vert
- Sources of truth: shaders/core/gbuffer.frag

## Notes

Vertex: world pos, normal, tangent, dual UV, current+prev clip for velocity.

Fragment MRT: albedo, packed normal, material params, motion vectors.

Bindless textures via nonuniformEXT; material push constants carry indices.

## Notes

Source map:
- `ohao/render/deferred/gbuffer_pass.hpp`
- `shaders/core/gbuffer.vert`
- `shaders/core/gbuffer.frag`
