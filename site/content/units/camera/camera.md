---
module: camera
id: camera
title: Camera
---

## What

Camera view/projection and FPS/orbit controls used by examples.

## How

camera.hpp/cpp feed CameraUniformBuffer each frame.

## Why

Examples share one camera model so jitter/TAA conventions stay consistent.

## Contracts

- Sources of truth: ohao/render/camera/camera.hpp
- Sources of truth: ohao/render/camera/camera.cpp

## Notes

camera.hpp: defines `class Camera`.

camera.hpp: defines `enum class ProjectionType`.

## Notes

Source map:
- `ohao/render/camera/camera.hpp`
- `ohao/render/camera/camera.cpp`
