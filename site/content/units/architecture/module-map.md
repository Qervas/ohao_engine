---
module: architecture
id: module-map
title: Module ownership map
---

## What

Ownership map: which directory may depend on which. Scene never sees Vulkan; GPU owns devices and uploads; render/* owns techniques; shaders mirror GPU bindings.

## How

ohao/core — types, EventBus, Result (no Vulkan)

ohao/scene — actors, components, loaders (no Vulkan)

ohao/gpu — device, bindless, upload, dispatch

ohao/render/* — deferred, RT, denoise, graph, camera

ohao/physics, ohao/audio — facades with pluggable backends

shaders/ — SPIR-V sources matching set/binding contracts

## Why

Forbidden edges prevent the classic 'everything includes renderer.hpp' collapse that freezes refactors.

## Contracts

- scene → core only (plus glm)
- gpu may include scene types for upload but not inverse
- Public monograph omits non-product research trees

## Notes

Scene never includes vulkan.h. Non-product research trees are omitted from this public face.

## Notes

Source map:
