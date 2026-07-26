"""Tree units: 01 · Architecture."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'architecture',
    "title": '01 · Architecture',
    "hub": 'architecture.html',
    "children": [
        page(
            'request-path',
            'Request path',
            'CLI/example → Scene → VulkanRenderer::initialize → setScene → render → pixels.',
            files=['examples/interactive.cpp', 'examples/cornell_box.cpp', 'ohao/gpu/vulkan/renderer.cpp'],
            design=['The engine is a library: no editor host required.', 'Examples are thin drivers over the same API surface.'],
            workflow=['Parse CLI (model, env, mode, denoise)', 'Build Scene + load assets into components', 'VulkanRenderer(w,h).initialize() cold start', 'setScene → upload + BLAS/TLAS', 'setRenderMode / ensureRTRenderer as needed', 'render() loop or single shot; getPixels / present'],
            why='One mental model for every tool (viewer, turntable, tests).',
        ),
        page(
            'invariants',
            'Cross-module invariants',
            'Contracts that break pixels silently if violated.',
            topics=['TLAS instance order == material row order', 'Bindless index 0xFFFFFFFF = no texture', 'Frame ring: wait fence before staging readback', 'NRD: YCoCg + norm hit-dist (not linear RGB)', 'Lazy PathTracer profiles (no dual 4K OOM)', 'Same PBR language for deferred and PT'],
            design=['These are product rules, not style preferences. Documented in STATUS and chapter contracts.'],
        ),
        page(
            'module-map',
            'Module ownership map',
            'Which directory owns which concern; forbidden dependencies.',
            topics=['ohao/core — types, events, Result (no Vulkan)', 'ohao/scene — actors/components/loaders (no Vulkan)', 'ohao/gpu — device, upload, AS, dispatch', 'ohao/render/* — formation techniques', 'ohao/physics, ohao/audio — facades', 'shaders/ — SPIR-V sources matching GPU bindings'],
            design=['Scene never includes vulkan.h. Non-product research trees are omitted from this public face.'],
        ),
    ],
}
