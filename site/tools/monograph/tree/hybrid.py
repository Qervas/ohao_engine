"""Tree units: 08 · Hybrid RT."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'hybrid',
    "title": '08 · Hybrid RT',
    "hub": 'hybrid.html',
    "children": [
        page(
            'shadow-technique',
            'RT shadow technique',
            'init → SBT → render visibility from GBuffer.',
            files=['ohao/render/rt/rt_shadow_technique.hpp', 'ohao/render/rt/rt_shadow_technique.cpp', 'shaders/rt/rt_shadow.rgen', 'shaders/rt/rt_shadow.rmiss', 'shaders/rt/rt_shadow.rahit'],
            design=['Trace visibility rays from GBuffer world positions toward lights.', 'Output shadow mask sampled in deferred lighting — soft RT shadows without full PT.'],
        ),
        page(
            'gi-technique',
            'RT GI technique',
            '1-bounce GI inject, material albedos.',
            files=['ohao/render/rt/rt_gi_technique.hpp', 'ohao/render/rt/rt_gi_technique.cpp', 'shaders/rt/rt_gi.rgen', 'shaders/rt/rt_gi.rchit', 'shaders/rt/rt_gi.rmiss'],
            design=['One-bounce GI from GBuffer normals; injects irradiance into deferred lighting.', 'Cheaper than full path tracer; shares TLAS with shadows.'],
        ),
        page(
            'visibility',
            'Visibility helpers',
            'Shared RT visibility utilities.',
            files=['ohao/render/rt/rt_visibility.hpp', 'ohao/render/rt/render_technique.hpp'],
            design=['RenderTechnique base pattern: init / resize / render / cleanup for hybrid RT passes.', 'rt_visibility helpers for common ray flags and miss shaders.'],
        ),
    ],
}
