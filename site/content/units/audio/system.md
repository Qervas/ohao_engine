---
module: audio
id: system
title: AudioSystem facade
---

## What

AudioSystem facade over miniaudio: SFX/Music/Ambient buses, handles, 3D listener, volume clamps.

## How

initialize → loadSound(path, category) → play / play3D → setCategoryVolume / setMasterVolume → shutdown

ma_engine/ma_sound forward-declared; SoundHandle 0 invalid.


- initialize
- loadSound
- play/play3D
- set volumes
- shutdown

## Why

Game-ready audio without OpenAL; one facade for tools and runtime.

## Contracts

- Sources of truth: ohao/audio/audio_system.hpp
- Sources of truth: ohao/audio/audio_system.cpp
- Sources of truth: ohao/audio/audio_module.hpp

## Notes

miniaudio under the hood; ma_engine/ma_sound forward-declared to keep headers light.

SoundCategory: SFX | Music | Ambient with per-bus volume clamps [0,1].

SoundHandle 0 = INVALID; play APIs are handle-based (no exceptions).

3D: setListener(pos, forward, up); positional play for SFX.

## Notes

Source map:
- `ohao/audio/audio_system.hpp`
- `ohao/audio/audio_system.cpp`
- `ohao/audio/audio_module.hpp`
