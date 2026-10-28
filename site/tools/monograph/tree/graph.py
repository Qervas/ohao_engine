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
            'culling',
            'GPU culling',
            'The draw table to indirect lists: early/late two-pass occlusion, four lists a G-buffer phase, Hi-Z, cascades.',
            files=['ohao/render/deferred/gpu_culling.hpp', 'ohao/render/deferred/gpu_culling.cpp', 'shaders/cull/instances.slang', 'shaders/cull/common.slang', 'shaders/cull/hiz.slang'],
        ),
        page(
            'meshlets',
            'Meshlets and mesh shaders',
            'The greedy meshlet builder, its arenas, task/mesh shaders and the vertex-pulling fallback.',
            files=['ohao/render/scene/meshlet_builder.hpp', 'ohao/render/scene/meshlet_builder.cpp', 'shaders/cull/meshlets.slang', 'shaders/cull/common.slang'],
        ),
        page(
            'frame-and-settings',
            'The frame, measured and set',
            'The pass registry in frame order, per-pass GPU timings in captures, and render.settings.',
            files=['ohao/render/frame/pass_registry.hpp', 'ohao/runtime/frame_capture.cpp', 'ohao/gpu/vulkan/renderer.cpp', 'ohao/api/session.cpp'],
            design=['kPasses: every pass a frame can run, in frame order; the timer, render.stats, capture timings and render_budgets.json share its names.', 'RenderSettings: quality, RT shadows and GI, SSAO/SSR/SSS, geometry, occlusion culling, CSM, exposure; a pass turned off is not registered.', 'render.settings: set merges onto the current settings, limits checked first, counts as a change; captures default to them.'],
            workflow=['render.settings set', 'frame (passes registered by settings)', 'capture reads back after the fence', 'gpuMs and timings'],
        ),
    ],
}
