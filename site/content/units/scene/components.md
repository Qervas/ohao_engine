---
module: scene
id: components
title: Components
---

## What

Components are pure data + light behavior: Mesh, Material, Light, Transform, Physics. Factories and packs spawn common kits.

## How

component.hpp base; specialized headers for mesh/material/light/transform.

component_factory / component_pack create primitives with sensible defaults for demos.

## Why

Upload and physics query components by type; no virtual draw() on actors.

## Contracts

- Sources of truth: ohao/scene/component/component.hpp
- Sources of truth: ohao/scene/component/mesh_component.hpp
- Sources of truth: ohao/scene/component/material_component.hpp
- Sources of truth: ohao/scene/component/light_component.hpp

## Notes

Composition over inheritance: renderer iterates MeshComponents, not class hierarchies.

## Notes

Source map:
- `ohao/scene/component/component.hpp`
- `ohao/scene/component/mesh_component.hpp`
- `ohao/scene/component/material_component.hpp`
- `ohao/scene/component/light_component.hpp`
- `ohao/scene/component/transform_component.hpp`
- `ohao/scene/component/component_factory.hpp`
- `ohao/scene/component/component_pack.hpp`
