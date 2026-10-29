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
            'Inline ray queries per light, filtered: 8 visibility channels for lighting.',
            files=['ohao/render/rt/rt_shadow_technique.hpp', 'ohao/render/rt/rt_shadow_technique.cpp', 'ohao/render/rt/rt_compute.hpp', 'ohao/render/rt/rt_compute.cpp', 'shaders/rt/shadow_trace.slang', 'shaders/rt/shadow_filter.slang'],
            design=['Compute trace with inline ray queries from GBuffer positions to up to 4 lights a pixel by importance (reference: 8 rays to every light).', 'Depth- and normal-aware filter plus temporal accumulation; one 8-bit visibility channel a light, read by deferred lighting.'],
        ),
        page(
            'gi-technique',
            'RT GI technique',
            'One-bounce diffuse GI: half-res trace, temporal + à-trous + upsample.',
            files=['ohao/render/rt/rt_gi_technique.hpp', 'ohao/render/rt/rt_gi_technique.cpp', 'ohao/render/rt/rt_compute.hpp', 'ohao/render/rt/rt_compute.cpp', 'shaders/rt/gi_trace.slang', 'shaders/rt/gi_denoise.slang'],
            design=['Compute trace with inline ray queries: half resolution, 2 cosine rays a pixel, hits lit by every light (one shadowed, chosen by what it gives), misses by the env map (reference: full resolution, 32 rays, running mean).', 'Denoised by a reprojected two-speed temporal accumulation, 2 à-trous iterations and a joint-bilateral upsample; lighting adds it at binding 11.'],
        ),
        page(
            'visibility',
            'Visibility helpers',
            'Shared RT visibility utilities.',
            files=['shaders/shared/layout.json', 'ohao/render/rt/render_technique.hpp'],
            design=['RenderTechnique base pattern: init / resize / render / cleanup for hybrid RT passes.', 'TLAS instance and ray masks: scene geometry and light emitters, camera rays and every other ray.'],
        ),
    ],
}
