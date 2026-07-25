---
module: scene
id: actors
title: Actors & hierarchy
---

## What

Actor is the scene graph node: identity, name, parent/children, lifecycle, and a bag of components. Transform is always present.

## How

- construct → addComponent → initialize → start → update → destroy
Hierarchy walks update world transforms before mesh/physics consumers read them.

Actor::Ptr is shared ownership from Scene maps.


- Transform always present

## Why

Composition over deep inheritance: renderers iterate MeshComponents, not class hierarchies.

## Contracts

- Transform always present on Actor
- Scene owns actor maps by id and name

## Notes

actor.hpp: defines `class Scene`.

actor.hpp: defines `class Component`.

actor.hpp: defines `class Model`.

scene_object.hpp: defines `class SceneObject`.

## Notes

Source map:
- `ohao/scene/actor/actor.hpp`
- `ohao/scene/actor/actor.cpp`
- `ohao/scene/scene_object.hpp`
