---
module: graph
id: particles
title: Particle system
---

## What

Particle system orchestration (CPU/GPU).

## How

particle_system ties compute emit/update to draw.

## Why

Effects without mesh component spam.

## Contracts

- Sources of truth: ohao/render/particles/particle_system.hpp
- Sources of truth: ohao/render/particles/particle_system.cpp

## Notes

particle_system.hpp: defines `enum class ParticleType`.

particle_system.hpp: defines `struct ParticleEmitterConfig`.

particle_system.hpp: defines `class ParticleSystem`.

particle_system.cpp: defines `struct GPUParticle`.

## Notes

Source map:
- `ohao/render/particles/particle_system.hpp`
- `ohao/render/particles/particle_system.cpp`
