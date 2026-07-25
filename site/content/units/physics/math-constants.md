---
module: physics
id: math-constants
title: Physics math & constants
---

## What

Shared physics scalars and math utils.

## How

physics_constants + physics_math.

## Why

Avoid magic numbers across forces and backend helpers.

## Contracts

- Sources of truth: ohao/physics/common/physics_constants.hpp
- Sources of truth: ohao/physics/common/physics_constants.cpp
- Sources of truth: ohao/physics/utils/physics_math.hpp
- Sources of truth: ohao/physics/utils/physics_math.cpp

## Notes

physics_constants.hpp: defines `enum class RigidBodyType`.

physics_constants.hpp: defines `struct PhysicsConfig`.

physics_math.hpp: defines `struct AABB`.

## Notes

Source map:
- `ohao/physics/common/physics_constants.hpp`
- `ohao/physics/common/physics_constants.cpp`
- `ohao/physics/utils/physics_math.hpp`
- `ohao/physics/utils/physics_math.cpp`
