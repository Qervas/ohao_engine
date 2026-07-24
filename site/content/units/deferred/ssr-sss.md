---
module: deferred
id: ssr-sss
title: SSR & SSS
---

## What

Experimental SSR (HiZ ray march) and SSS blur for skin-like materials — quality gated on the deferred hub.

## How

ssr.comp / sss_blur.comp with pass wrappers.

## Why

Optional quality; not required for cornell golden path.

## Contracts

- Sources of truth: ohao/render/deferred/ssr_pass.hpp
- Sources of truth: ohao/render/deferred/sss_pass.hpp
- Sources of truth: shaders/postprocess/ssr.comp
- Sources of truth: shaders/postprocess/sss_blur.comp

## Notes

SSR ray-marches HiZ for glossy reflections; SSS blur for skin-like materials — quality flags on hub.

## Notes

Source map:
- `ohao/render/deferred/ssr_pass.hpp`
- `ohao/render/deferred/sss_pass.hpp`
- `shaders/postprocess/ssr.comp`
- `shaders/postprocess/sss_blur.comp`
