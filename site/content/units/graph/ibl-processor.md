---
module: graph
id: ibl-processor
title: IBL processor
---

## What

IBL bake: equirect→cubemap, prefilter, BRDF LUT.

## How

ibl_processor dispatches compute shaders when env changes.

## Why

Runtime IBL without offline content pipeline dependency.

## Contracts

- Sources of truth: ohao/render/ibl/ibl_processor.hpp
- Sources of truth: ohao/render/ibl/ibl_processor.cpp

## Notes

ibl_processor.hpp: defines `class IBLProcessor`.

## Notes

Source map:
- `ohao/render/ibl/ibl_processor.hpp`
- `ohao/render/ibl/ibl_processor.cpp`
