---
module: graph
id: picking
title: Picking
---

## What

Ray pick helpers for selection.

## How

picking_system + ray.hpp.

## Why

Tools need object pick independent of render mode.

## Contracts

- Sources of truth: ohao/render/picking/picking_system.hpp
- Sources of truth: ohao/render/picking/ray.hpp

## Notes

picking_system.hpp: defines `class PickingSystem`.

ray.hpp: defines `struct Ray`.

ray.hpp: defines `struct PickResult`.

## Notes

Source map:
- `ohao/render/picking/picking_system.hpp`
- `ohao/render/picking/ray.hpp`
