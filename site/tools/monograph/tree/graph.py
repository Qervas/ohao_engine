"""Tree units: Render graph."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'graph',
    "title": 'Render graph',
    "hub": None,
    "children": [
        page(
            'render-graph',
            'RenderGraph',
            'Declare passes, barriers, compile order.',
            files=['ohao/render/graph/render_graph.hpp', 'ohao/render/graph/render_graph.cpp', 'ohao/render/graph/render_pass.hpp', 'ohao/render/graph/resource_handle.hpp'],
            design=['Deferred uses graph for CSM→GBuffer→SSAO→Lighting barriers while passes own VkRenderPass objects.'],
        ),
        page(
            'frame-resources',
            'Frame resources ring',
            'MAX_FRAMES_IN_FLIGHT=3, fences, staging, per-frame UBOs.',
            files=['ohao/render/frame/frame_resources.hpp', 'ohao/render/frame/frame_resources.cpp'],
        ),
        page(
            'async-compute',
            'Async compute queue',
            'Optional async compute path.',
            files=['ohao/render/async/async_compute_queue.hpp', 'ohao/render/async/async_compute_queue.cpp'],
        ),
        page(
            'ibl-processor',
            'IBL processor',
            'Env map processing (cubemap/prefilter) helpers.',
            files=['ohao/render/ibl/ibl_processor.hpp', 'ohao/render/ibl/ibl_processor.cpp'],
        ),
        page(
            'particles',
            'Particle system',
            'CPU/GPU particle orchestration.',
            files=['ohao/render/particles/particle_system.hpp', 'ohao/render/particles/particle_system.cpp'],
        ),
        page(
            'picking',
            'Picking',
            'Ray pick helpers for selection.',
            files=['ohao/render/picking/picking_system.hpp', 'ohao/render/picking/ray.hpp'],
        ),
        page(
            'culling',
            'Culling',
            'CPU/GPU cull helpers (HiZ / light cull compute).',
            files=['ohao/render/culling.hpp', 'shaders/compute/gpu_cull.comp', 'shaders/compute/hiz_generate.comp'],
        ),
    ],
}
