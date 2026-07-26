---
module: physics
id: world
title: PhysicsWorld
---

## What

PhysicsWorld: config (gravity, dt, substeps, CCD), SimulationState machine, body registry, profiles.

## How

step(dt); sync transforms to components; ProfileManager presets.


- construct
- add bodies/forces
- step
- sync transforms

## Why

One world per scene; profiles tune quality without code forks.

## Contracts

- Sources of truth: ohao/physics/world/physics_world.hpp
- Sources of truth: ohao/physics/world/physics_world.cpp
- Sources of truth: ohao/physics/world/physics_settings.hpp
- Sources of truth: ohao/physics/world/profile_manager.hpp

## Notes

PhysicsWorldConfig: gravity, dt, substeps, sleeping, CCD, body caps.

SimulationState: STOPPED | RUNNING | PAUSED | STEPPING.

ProfileManager + SimulationProfile tune quality/perf presets.

## Notes

Source map:
- `ohao/physics/world/physics_world.hpp`
- `ohao/physics/world/physics_world.cpp`
- `ohao/physics/world/physics_settings.hpp`
- `ohao/physics/world/profile_manager.hpp`
- `ohao/physics/world/profile_manager.cpp`
- `ohao/physics/world/simulation_profile.hpp`
- `ohao/physics/world/simulation_profile.cpp`
