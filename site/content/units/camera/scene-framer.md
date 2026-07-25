---
module: camera
id: scene-framer
title: Scene framer
---

## What

Auto-frame scene bounds for turntable/model viewer.

## How

scene_framer computes distance/FOV from AABB.

## Why

Demos should open on a well-framed model without manual tuning.

## Contracts

- Sources of truth: ohao/render/camera/scene_framer.hpp
- Sources of truth: ohao/render/camera/scene_framer.cpp

## Notes

scene_framer.hpp: defines `class Scene`.

scene_framer.hpp: defines `class Camera`.

scene_framer.hpp: defines `struct Vertex`.

SceneFramer — auto-frame any model with correct camera, lights, and room size.

Analyzes model AABB, scales to fit, and positions camera + lights automatically.

Works for ANY model size (mm, cm, m, arbitrary units).

## Notes

Source map:
- `ohao/render/camera/scene_framer.hpp`
- `ohao/render/camera/scene_framer.cpp`
