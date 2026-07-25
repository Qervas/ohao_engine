---
module: systems
id: tests
title: Tests & goldens
---

## What

Unit suites + golden image gate for PT/deferred regressions.

## How

tests/engine, tests/renderer, tests/golden.

## Why

Visual regressions are the real product test for a renderer.

## Contracts

- Sources of truth: tests/CMakeLists.txt
- Sources of truth: tests/golden
- Sources of truth: tests/engine
- Sources of truth: tests/renderer

## Notes

Golden images gate path-tracer and deferred regressions.

Engine/renderer unit tests cover upload, materials, AS invariants.

## Notes

Source map:
- `tests/CMakeLists.txt`
- `tests/golden`
- `tests/engine`
- `tests/renderer`
