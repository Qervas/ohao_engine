---
module: physics
id: bodies
title: Bodies & shapes
---

## What

RigidBody + shape hierarchy (box/sphere/capsule/cylinder/plane/triangle mesh) + PhysicsComponent on actors.

## How

ShapeFactory builds shapes; component binds body to Actor.

## Why

Uniform body API over backend-specific types.

## Contracts

- Sources of truth: ohao/physics/dynamics/rigid_body.hpp
- Sources of truth: ohao/physics/dynamics/rigid_body.cpp
- Sources of truth: ohao/physics/collision/shapes/collision_shape.hpp
- Sources of truth: ohao/physics/collision/shapes/shape_factory.hpp

## Notes

Shape hierarchy + ShapeFactory for box/sphere/capsule/cylinder/plane/triangle mesh.

PhysicsComponent binds a body to an Actor; Scene owns the world.

## Notes

Source map:
- `ohao/physics/dynamics/rigid_body.hpp`
- `ohao/physics/dynamics/rigid_body.cpp`
- `ohao/physics/collision/shapes/collision_shape.hpp`
- `ohao/physics/collision/shapes/shape_factory.hpp`
- `ohao/physics/collision/shapes/box_shape.hpp`
- `ohao/physics/collision/shapes/sphere_shape.hpp`
- `ohao/physics/collision/shapes/capsule_shape.hpp`
- `ohao/physics/collision/shapes/cylinder_shape.hpp`
- `ohao/physics/collision/shapes/plane_shape.hpp`
- `ohao/physics/collision/shapes/triangle_mesh_shape.hpp`
- `ohao/physics/components/physics_component.hpp`
- `ohao/physics/components/physics_component.cpp`
