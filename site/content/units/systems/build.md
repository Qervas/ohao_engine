---
module: systems
id: build
title: Build system
---

## What

CMake options gate NRD, OIDN, DLSS, Jolt, tests; shaders custom target compiles GLSL→SPIR-V.

## How

Top-level + ohao + examples + shaders CMakeLists.

## Why

Optional heavy deps must not break bare builds.

## Contracts

- Sources of truth: CMakeLists.txt
- Sources of truth: ohao/CMakeLists.txt
- Sources of truth: examples/CMakeLists.txt
- Sources of truth: shaders/CMakeLists.txt

## Notes

Top-level options gate NRD, OIDN, DLSS, Jolt, tests.

shaders custom target compiles GLSL → SPIR-V into bin/shaders.

examples link ohao libraries; interactive is the full-featured driver.

## Notes

Source map:
- `CMakeLists.txt`
- `ohao/CMakeLists.txt`
- `examples/CMakeLists.txt`
- `shaders/CMakeLists.txt`
