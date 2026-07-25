---
module: path-tracer
id: profiles
title: Profiles & meta
---

## What

RTRenderSettings profiles: kRealtime vs kOffline — spp, bounces, sampler type, denoise defaults.

## How

rt_settings.hpp / rt_meta.hpp traits; rt_profile_renderer bridges mode dispatch.

## Why

Interactive and reference share host code; only knobs change.

## Contracts

- Sources of truth: ohao/render/rt/rt_settings.hpp
- Sources of truth: ohao/render/rt/rt_meta.hpp
- Sources of truth: ohao/render/rt/rt_profile_renderer.hpp

## Notes

kRealtimeRTSettings vs kOfflineRTSettings: spp, max bounce, sampler type, denoise defaults.

rt_meta traits drive compile-time and runtime feature gates (ReSTIR planes, NRD packing).

Never construct both realtime and offline PathTracer at 4K without explicit opt-in — VRAM doubles.

## Notes

Source map:
- `ohao/render/rt/rt_settings.hpp`
- `ohao/render/rt/rt_meta.hpp`
- `ohao/render/rt/rt_profile_renderer.hpp`
