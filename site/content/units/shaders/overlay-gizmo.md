---
module: shaders
id: overlay-gizmo
title: Overlay gizmo shaders
---

## What

Overlay gizmo vert/frag for debug draws.

## How

Depth-aware lines/solids after main stack.

## Why

Editor visibility without GBuffer pollution.

## Contracts

- Sources of truth: shaders/overlay/gizmo.vert
- Sources of truth: shaders/overlay/gizmo.frag

## Notes

Source map:
- `shaders/overlay/gizmo.vert`
- `shaders/overlay/gizmo.frag`
