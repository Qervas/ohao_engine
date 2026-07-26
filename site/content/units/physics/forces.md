---
module: physics
id: forces
title: Force generators
---

## What

Force generators: gravity, spring, drag, volumes, fields, presets, registry.

## How

ForceRegistry maps generators→bodies each step; force_system_example.hpp is a usage sketch.

## Why

Gameplay forces without rewriting the integrator.

## Contracts

- Sources of truth: ohao/physics/forces/force_registry.hpp
- Sources of truth: ohao/physics/forces/force_registry.cpp
- Sources of truth: ohao/physics/forces/forces.hpp
- Sources of truth: ohao/physics/forces/force_generator.hpp

## Notes

ForceRegistry maps generators → bodies each step.

Volumes/fields for wind, attractors; presets for common setups.

force_system_example.hpp is a documented usage sketch, not a binary.

## Notes

Source map:
- `ohao/physics/forces/force_registry.hpp`
- `ohao/physics/forces/force_registry.cpp`
- `ohao/physics/forces/forces.hpp`
- `ohao/physics/forces/force_generator.hpp`
- `ohao/physics/forces/force_generator.cpp`
- `ohao/physics/forces/gravity_force.hpp`
- `ohao/physics/forces/gravity_force.cpp`
- `ohao/physics/forces/spring_force.hpp`
- `ohao/physics/forces/spring_force.cpp`
- `ohao/physics/forces/drag_force.hpp`
- `ohao/physics/forces/drag_force.cpp`
- `ohao/physics/forces/force_volume.hpp`
- `ohao/physics/forces/force_volume.cpp`
- `ohao/physics/forces/field_force.hpp`
- `ohao/physics/forces/field_force.cpp`
- `ohao/physics/forces/environmental_force.hpp`
- `ohao/physics/forces/environmental_force.cpp`
- `ohao/physics/forces/force_presets.cpp`
- `ohao/physics/examples/force_system_example.hpp`
