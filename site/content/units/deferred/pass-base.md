---
module: deferred
id: pass-base
title: RenderPassBase
---

## What

RenderPassBase: shared init/cleanup/shader path helpers.

## How

Each pass only declares attachments and record().

## Why

DRY pipeline creation across a dozen passes.

## Contracts

- Sources of truth: ohao/render/deferred/render_pass_base.hpp
- Sources of truth: ohao/render/deferred/render_pass_base.cpp

## Notes

Common Vk pipeline/layout/shader load so each pass only declares attachments and record().

## Notes

Source map:
- `ohao/render/deferred/render_pass_base.hpp`
- `ohao/render/deferred/render_pass_base.cpp`
