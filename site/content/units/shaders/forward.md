---
module: shaders
id: forward
title: Forward shaders
---

## What

Legacy forward.vert/frag with limited lights.

## How

Used by RenderMode::Forward smoke paths.

## Why

Simple path for debugging without GBuffer.

## Contracts

- Sources of truth: shaders/core/forward.vert
- Sources of truth: shaders/core/forward.frag

## Notes

8-light-limit forward path for smoke tests and simple scenes.

## Notes

Source map:
- `shaders/core/forward.vert`
- `shaders/core/forward.frag`
