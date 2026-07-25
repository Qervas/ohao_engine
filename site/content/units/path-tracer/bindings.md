---
module: path-tracer
id: bindings
title: Descriptor map 0–35
---

## What

Descriptor set 0 map bindings 0–35: TLAS, accum, materials, lights, env CDF, AOVs, ReSTIR 29–34, DLSS hit-dist 35.

## How

path_tracer_descriptors.cpp writes all slots; GLSL layout(set=0,binding=N) must match exactly.

## Why

Wrong binding = silent black. Plate SVG documents the shipped map.

## Contracts

- 0 TLAS, 1 accum, 2 output, 3 materials, 11 lights
- 29–34 ReSTIR, 35 DLSS hit-dist when enabled

## Notes

Binding 0: TLAS. 1–2: accum + output. 3: materials. 11: GPULight SSBO.

17–18: env CDF. 19–26: motion, depth, roughness, split radiance/albedo AOVs.

29–34: ReSTIR reservoir ping-pong images (realtime).

35: DLSS hit-distance guide when DenoiseMode::DLSSRR.

Descriptor writes must match GLSL layout(set=0, binding=N) exactly — silent black if wrong.

## Notes

Source map:
- `ohao/render/rt/path_tracer_descriptors.cpp`
- `site/assets/plates/pt_bindings.svg`
