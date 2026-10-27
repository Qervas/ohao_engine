"""Tree units: 14 · Runtime feel (sub-project 9)."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'runtime',
    "title": '14 · Runtime feel',
    "hub": None,
    "children": [
        page(
            'feel',
            'Feel: the look lead, frame timing and the first-person gun',
            'The look no step has taken drawn through the step\'s own arithmetic, present-confirmed latency and pacing, a view-space gun that never shadows, and sparks and sounds from the combat log.',
            files=['ohao/runtime/frame_log.hpp', 'ohao/runtime/frame_log.cpp', 'ohao/gpu/vulkan/present_waiter.hpp',
                   'ohao/gpu/vulkan/present_waiter.cpp', 'ohao/ecs/view_model.hpp', 'ohao/ecs/view_model.cpp',
                   'ohao/runtime/combat_fx.hpp', 'ohao/runtime/combat_fx.cpp', 'ohao/runtime/game_audio.hpp',
                   'ohao/runtime/game_audio.cpp', 'tools/bench/feel_bench.cpp'],
            design=['The look lead: the interpolate sync system draws the player turned by the pending look through applyLook.', 'FrameLog stamps poll, view, submit and present; present wait confirms the present.', 'ViewModel: S = diag(d, d, d/k) in view space, depthScale 0.55, in no cascade and seen only by camera rays.'],
        ),
    ],
}
