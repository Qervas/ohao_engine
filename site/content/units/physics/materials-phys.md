---
module: physics
id: materials-phys
title: Physics materials
---

## What

Friction/restitution physics materials.

## How

physics_material.hpp/cpp applied at body create.

## Why

Artist-tunable contact response separate from render materials.

## Contracts

- Sources of truth: ohao/physics/material/physics_material.hpp
- Sources of truth: ohao/physics/material/physics_material.cpp

## Notes

physics_material.hpp: defines `class PhysicsMaterial`.

physics_material.hpp: defines `enum class CombineMode`.

physics_material.hpp: defines `class MaterialLibrary`.

## Notes

Source map:
- `ohao/physics/material/physics_material.hpp`
- `ohao/physics/material/physics_material.cpp`
