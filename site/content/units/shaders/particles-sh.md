---
module: shaders
id: particles-sh
title: Particle shaders
---

## What

Particle emit/update compute + render shaders.

## How

Owned by particle_system pass.

## Why

GPU particles stay off the deferred geometry path.

## Contracts

- Sources of truth: shaders/particles/particle_emit.comp
- Sources of truth: shaders/particles/particle_update.comp
- Sources of truth: shaders/particles/particle_render.vert
- Sources of truth: shaders/particles/particle_render.frag

## Notes

particle_emit.comp: defines `struct Particle`.

Particle Emit Compute Shader Spawns new particles at emitter positions with randomized properties

particle_update.comp: defines `struct Particle`.

Particle Update Compute Shader Integrates velocity, applies forces, decrements lifetime

particle_render.vert: defines `struct Particle`.

Particle Render Vertex Shader Generates camera-facing billboard quads from particle data

Particle Render Fragment Shader Soft circular particles with alpha blending

## Notes

Source map:
- `shaders/particles/particle_emit.comp`
- `shaders/particles/particle_update.comp`
- `shaders/particles/particle_render.vert`
- `shaders/particles/particle_render.frag`
