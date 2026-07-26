"""Tree units: 10 · Audio."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'audio',
    "title": '10 · Audio',
    "hub": 'audio.html',
    "children": [
        page(
            'system',
            'AudioSystem facade',
            'Init, categories, handles, 3D, volumes.',
            files=['ohao/audio/audio_system.hpp', 'ohao/audio/audio_system.cpp', 'ohao/audio/audio_module.hpp'],
            design=['miniaudio under the hood; ma_engine/ma_sound forward-declared to keep headers light.', 'SoundCategory: SFX | Music | Ambient with per-bus volume clamps [0,1].', 'SoundHandle 0 = INVALID; play APIs are handle-based (no exceptions).', '3D: setListener(pos, forward, up); positional play for SFX.'],
            workflow=['initialize()', 'loadSound(path, category) → handle', 'play / play3D / stop', 'setCategoryVolume / setMasterVolume', 'shutdown'],
            why='Game-ready audio without pulling OpenAL; one facade for tools and runtime.',
        ),
    ],
}
