---
module: path-tracer
id: hit-miss
title: Hit & miss shaders
---

## What

closesthit: barycentrics → material → PBR payload; anyhit: alpha cutout; miss: environment contribution.

## How

pt_closesthit.rchit, pt_anyhit.rahit, pt_miss.rmiss share material unpack with raygen.

## Why

Split shader stages keep anyhit cheap and miss free of material state.

## Contracts

- Sources of truth: shaders/rt/pt_closesthit.rchit
- Sources of truth: shaders/rt/pt_anyhit.rahit
- Sources of truth: shaders/rt/pt_miss.rmiss

## Notes

closesthit: barycentrics → material row → PBR params into payload.

anyhit: alpha-tested geometry; ignoreIntersectionEXT when cutout fails.

miss: environment map / constant sky contribution into path radiance.

## Notes

Source map:
- `shaders/rt/pt_closesthit.rchit`
- `shaders/rt/pt_anyhit.rahit`
- `shaders/rt/pt_miss.rmiss`
