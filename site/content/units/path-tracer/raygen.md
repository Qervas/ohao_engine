---
module: path-tracer
id: raygen
title: Raygen integrator
---

## What

Raygen integrator family: pt_raygen.rgen (brute-force reference), realtime/offline variants with NEE, dual-lobe BRDF, MIS, AOV packing for denoisers.

## How

Primary ray from camera UBO; closest-hit payload carries throughput.

NEE: sample light (sphere solid angle), shadow ray with Tmax = lightDist (not radius).

BRDF sample next direction; Russian roulette / max bounce.

Write beauty + AOVs; NRD path packs YCoCg + norm hit-dist via nrd_frontend.glsl.


- Camera primary ray
- Closest-hit payload
- NEE + shadow ray
- BRDF sample bounce
- RR / max depth
- Accum + AOV writes

## Why

One integrator family; profile swaps spp/sampler without forking the host API. Hub NEE walk anchors L305+ line pedagogy.

## Contracts

- Shadow Tmax uses distance to light sample, not sphere radius
- NRD inputs are YCoCg+hitDist, not linear RGB

## Math

L_o = L_e + \int_{\mathcal{H}} f_r L_i |n\cdot w_i|\,dw_i

\hat{L}_{\mathrm{NEE}} = \frac{f_r L_e |n\cdot w_i|}{p_{\mathrm{light}}} V

## Notes

pt_raygen.rgen is the brute-force reference: camera ray → bounce → light hit, no NEE.

Realtime/offline variants add NEE (next-event estimation), dual-lobe BRDF sampling, and MIS.

NEE sphere sample: sample light disk by solid angle, shadow ray with d=lightDist (not radius) for Tmin/Tmax.

AOV writes: albedo, normal, depth, roughness, diffuse/specular radiance, motion vectors for denoisers.

NRD path packs radiance as YCoCg + normalized hit-distance before REBLUR (see nrd_frontend.glsl).

Hub monograph has the line-anchored NEE walk (L305+) with GLSL and why-notes.

## Notes

Source map:
- `shaders/rt/pt_raygen.rgen`
- `shaders/rt/pt_raygen_realtime.rgen`
- `shaders/rt/pt_raygen_offline.rgen`
