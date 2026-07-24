---
module: deferred
id: gizmo
title: Gizmo pass
---

## What

Debug/editor overlay meshes after the main stack.

## How

gizmo_pass + gizmo_meshes + overlay shaders.

## Why

Selection and force viz without polluting GBuffer.

## Contracts

- Sources of truth: ohao/render/deferred/gizmo_pass.hpp
- Sources of truth: ohao/render/gizmo/gizmo_meshes.hpp
- Sources of truth: shaders/overlay/gizmo.vert

## Notes

Line/solid debug meshes drawn after tonemap path for selection and force viz.

## Notes

Source map:
- `ohao/render/deferred/gizmo_pass.hpp`
- `ohao/render/gizmo/gizmo_meshes.hpp`
- `shaders/overlay/gizmo.vert`
