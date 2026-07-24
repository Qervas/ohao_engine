---
module: hybrid
id: visibility
title: Visibility helpers
---

## What

Shared RenderTechnique pattern and visibility helpers for hybrid passes.

## How

init / resize / render / cleanup lifecycle.

## Why

Uniform lifecycle keeps DeferredRenderer orchestration simple.

## Contracts

- Sources of truth: ohao/render/rt/rt_visibility.hpp
- Sources of truth: ohao/render/rt/render_technique.hpp

## Notes

RenderTechnique base pattern: init / resize / render / cleanup for hybrid RT passes.

rt_visibility helpers for common ray flags and miss shaders.

## Notes

Source map:
- `ohao/render/rt/rt_visibility.hpp`
- `ohao/render/rt/render_technique.hpp`
