---
module: core
id: command-result
title: Command history & Result
---

## What

Command history supports undo/redo editor-style operations; Result<T,E> is the fallible return type for loaders and init paths that should not throw on expected failures.

## How

Command interface execute/undo; history stack in command.cpp.

Result exposes ok/err accessors and monadic helpers without exceptions on the hot path.

## Why

Load failures and missing files are expected; exceptions are for bugs. Undo is optional tooling, not render-critical.

## Contracts

- Sources of truth: ohao/core/command.hpp
- Sources of truth: ohao/core/command.cpp
- Sources of truth: ohao/core/result.hpp

## Notes

Result avoids exceptions on expected failures in loaders and init paths.

## Notes

Source map:
- `ohao/core/command.hpp`
- `ohao/core/command.cpp`
- `ohao/core/result.hpp`
