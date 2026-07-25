---
module: physics
id: backend
title: Backend plugin
---

## What

IPhysicsBackend plugin: factory picks Jolt or null; helpers convert shapes without leaking Jolt into scene headers.

## How

backend_factory.cpp; jolt_backend.*; jolt_helpers.hpp.

## Why

Scene/physics API stays backend-agnostic for tests and optional builds.

## Contracts

- Sources of truth: ohao/physics/backend/physics_backend.hpp
- Sources of truth: ohao/physics/backend/backend_factory.cpp
- Sources of truth: ohao/physics/backend/jolt/jolt_backend.hpp
- Sources of truth: ohao/physics/backend/jolt/jolt_backend.cpp

## Notes

IPhysicsBackend abstracts create/step/destroy; factory picks Jolt or null.

jolt_helpers convert OHAO shapes/math into Jolt types without leaking Jolt into scene headers.

## Notes

Source map:
- `ohao/physics/backend/physics_backend.hpp`
- `ohao/physics/backend/backend_factory.cpp`
- `ohao/physics/backend/jolt/jolt_backend.hpp`
- `ohao/physics/backend/jolt/jolt_backend.cpp`
- `ohao/physics/backend/jolt/jolt_helpers.hpp`
- `ohao/physics/physics_module.hpp`
