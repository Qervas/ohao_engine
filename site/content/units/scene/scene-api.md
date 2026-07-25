---
module: scene
id: scene-api
title: Scene API
---

## What

Scene owns actors, registries, optional PhysicsWorld, tags, and descriptor metadata for serialization hooks.

## How

createActor / createActorWithComponents / addActor / removeActor

find by id, name, or tag; forEach helpers for non-hot iteration

update(dt) walks actors then steps physics if present


- createActor / createActorWithComponents(primitive)
- addActor / removeActor
- update(dt) walks actors → components
- Physics world step if present

## Why

One ownership root prevents dangling components and double physics worlds.

## Contracts

- Sources of truth: ohao/scene/scene.hpp
- Sources of truth: ohao/scene/scene.cpp
- Sources of truth: ohao/scene/default_scene_factory.hpp
- Sources of truth: ohao/scene/default_scene_factory.cpp

## Notes

Scene owns Actor maps (by id and name), mesh/physics registries, and optional PhysicsWorld.

string_view at query boundaries; owned strings in maps. Tags via findActorsByTag.

scene_module.hpp is the module umbrella include for consumers.

## Notes

Source map:
- `ohao/scene/scene.hpp`
- `ohao/scene/scene.cpp`
- `ohao/scene/default_scene_factory.hpp`
- `ohao/scene/default_scene_factory.cpp`
- `ohao/scene/scene_module.hpp`
