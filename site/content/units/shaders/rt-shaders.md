---
module: shaders
id: rt-shaders
title: RT program family
---

## What

Full RT program family: path tracer stages, hybrid shadow/GI, NRD compose, SVGF/à-trous, cinematic post.

## How

pt_* for full PT; rt_shadow_* / rt_gi_* for hybrid; compute post for denoise/cinematic.

## Why

One SPIR-V family documented together so binding/SBT changes stay coordinated.

## Contracts

- Sources of truth: shaders/rt/pt_raygen.rgen
- Sources of truth: shaders/rt/pt_raygen_realtime.rgen
- Sources of truth: shaders/rt/pt_raygen_offline.rgen
- Sources of truth: shaders/rt/pt_closesthit.rchit

## Notes

pt_raygen.rgen = reference brute force; realtime/offline variants add NEE/MIS/AOV packing.

Hybrid: rt_shadow_* visibility; rt_gi_* one-bounce inject into deferred.

Post-RT: NRD compose unpack, optional SVGF/à-trous, cinematic bloom/DoF.

## Notes

Source map:
- `shaders/rt/pt_raygen.rgen`
- `shaders/rt/pt_raygen_realtime.rgen`
- `shaders/rt/pt_raygen_offline.rgen`
- `shaders/rt/pt_closesthit.rchit`
- `shaders/rt/pt_anyhit.rahit`
- `shaders/rt/pt_miss.rmiss`
- `shaders/rt/rt_shadow.rgen`
- `shaders/rt/rt_shadow.rmiss`
- `shaders/rt/rt_shadow.rahit`
- `shaders/rt/rt_gi.rgen`
- `shaders/rt/rt_gi.rchit`
- `shaders/rt/rt_gi.rmiss`
- `shaders/rt/nrd_compose.comp`
- `shaders/rt/rt_atrous.comp`
- `shaders/rt/rt_svgf_temporal.comp`
- `shaders/rt/rt_svgf_atrous.comp`
- `shaders/rt/cinematic_bloom_extract.comp`
- `shaders/rt/cinematic_bloom_blur.comp`
- `shaders/rt/cinematic_dof.comp`
- `shaders/rt/cinematic_composite.comp`
