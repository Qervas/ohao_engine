---
module: path-tracer
id: restir
title: ReSTIR GI reservoirs
---

## What

ReSTIR GI reservoirs as set-0 storage images for realtime light sample reuse.

## How

Ping-pong planes on bindings 29–34; offline may skip and use Sobol spp instead.

## Why

Realtime GI needs temporal reuse; reservoirs keep it inside the same descriptor set as PT.

## Contracts

- Sources of truth: ohao/render/rt/path_tracer_descriptors.cpp

## Notes

Realtime GI resampling stores light samples in reservoir images for temporal reuse.

Planes live in set-0 as storage images — same descriptor set as the path tracer.

Offline profile may skip ReSTIR and rely on spp + Sobol instead.

## Notes

Source map:
- `ohao/render/rt/path_tracer_descriptors.cpp`
