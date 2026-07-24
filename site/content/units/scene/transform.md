---
module: scene
id: transform
title: Transform math
---

## What

TRS transform math: local matrix from translation/rotation/scale; world matrix via parent chain. Upload and TLAS read world only.

## How

Dirty flags recompute world matrices on demand during update or upload.

Used by MeshComponent world poses and physics sync.

## Why

TLAS instances need stable world matrices; recomputing every frame without dirty flags is free performance loss.

## Contracts

- Sources of truth: ohao/scene/transform.hpp
- Sources of truth: ohao/scene/transform.cpp

## Notes

Local TRS → local matrix; parent chain → world matrix.

Upload and TLAS instance transforms read world matrices only — dirty flags matter.

## Notes

Source map:
- `ohao/scene/transform.hpp`
- `ohao/scene/transform.cpp`
