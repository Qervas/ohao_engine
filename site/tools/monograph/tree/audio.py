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
        page(
            'schedule',
            'The voice schedule',
            'Every sound decided in the step; the device only renders it.',
            files=['ohao/ecs/audio_schedule.hpp', 'ohao/ecs/audio_schedule.cpp', 'ohao/runtime/audio_player.cpp', 'ohao/api/audio.cpp'],
            design=['The step\'s last simulate system turns physics, combat and animation events into voice commands.', 'Priority, caps and holdMs pick what plays; ducks and the round\'s snapshot are commands too.', 'Runtime state, never hashed; it rides the World\'s snapshot through a SnapshotHook.', 'A window\'s AudioPlayer plays the commands a step later on a device clock; agents read them through audio.info.'],
            workflow=['installAudio(world, assets)', 'world.step', 'audioSchedule(world).commandsSince(since, max, dropped)', 'AudioPlayer::submit', 'audio.info / audio.mix'],
            why='A sound decided from the step is the same in a window, headless and in a replay, so it can be tested without a device.',
        ),
    ],
}
