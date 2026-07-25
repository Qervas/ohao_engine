"""Tree units: 03 · GPU / Vulkan."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'gpu',
    "title": '03 · GPU / Vulkan',
    "hub": 'gpu.html',
    "children": [
        page(
            'renderer-facade',
            'VulkanRenderer facade',
            'Public API: modes, setScene, ensureRT, denoise, pixels, present.',
            files=['ohao/gpu/vulkan/renderer.hpp', 'ohao/gpu/vulkan/renderer.cpp', 'ohao/gpu/vulkan/renderer_impl.hpp', 'ohao/gpu/gpu_module.hpp'],
            design=['RenderMode: Forward | Deferred | RTRealtime | RTOffline. isRTRenderMode / isRasterRenderMode helpers.', 'PathTracer is lazy (ensureRTRenderer) so deferred-only sessions avoid dual 4K RT OOM.', 'renderer_impl.hpp holds private split of large renderer translation units.', 'CameraUniformBuffer and SimpleVertex are GpuPod-asserted shared layouts.'],
            workflow=['VulkanRenderer(w,h).initialize()', 'setScene → upload + BLAS/TLAS', 'setRenderMode / ensureRTRenderer', 'setDenoiseMode (None|OIDN|NRD|Atrous|DLSSRR)', 'render() → getPixels / present'],
            why='One facade for every example and test; modes share scene upload and AS.',
        ),
        page(
            'init',
            'Device init',
            'createInstance → pickPhysicalDevice → createLogicalDevice feature chain.',
            files=['ohao/gpu/vulkan/device_setup.cpp', 'ohao/gpu/vulkan/renderer.cpp'],
            design=['Feature chain must enable accelerationStructure, rayTracingPipeline, bufferDeviceAddress, descriptorIndexing.', 'Without those, RT path fails at pipeline create — not at first draw.'],
            workflow=['Vulkan 1.3 instance', 'Prefer discrete GPU + graphics queue', 'Enable AS + RT pipeline + descriptor indexing + BDA', 'Optional DLSS device exts', 'Command pool, sync, frame resources'],
        ),
        page(
            'bindless',
            'Bindless textures',
            'BindlessTextureManager: variable count array, update-after-bind.',
            files=['ohao/gpu/vulkan/bindless_texture_manager.hpp', 'ohao/gpu/vulkan/bindless_texture_manager.cpp'],
            design=['Variable-count sampled image array with UPDATE_AFTER_BIND.', 'Materials store integer indices; shaders use nonuniformEXT.', 'Index 0xFFFFFFFF = unbound — never sample without a valid index.'],
            why='Avoid per-material descriptor set thrash on multi-texture GLBs.',
        ),
        page(
            'buffers-alloc',
            'Buffers & allocator',
            'VMA-style allocation, staging, UBOs, GPU allocator helpers.',
            files=['ohao/gpu/vulkan/gpu_allocator.hpp', 'ohao/gpu/vulkan/gpu_allocator.cpp', 'ohao/gpu/vulkan/buffer_setup.cpp', 'ohao/gpu/vulkan/vk_utils.hpp'],
            design=['GpuAllocator wraps device memory for buffers/images used by graph and PT.', 'Staging buffers for CPU→GPU mesh/material uploads; ring-friendly lifetimes.', 'vk_utils: create helpers, debug names, barrier one-liners.'],
        ),
        page(
            'scene-upload',
            'Scene upload',
            'Meshes → VB/IB map; materials; lights; env.',
            files=['ohao/gpu/vulkan/scene_upload.cpp', 'ohao/gpu/vulkan/light_upload.cpp'],
            design=['Walks MeshComponents → interleaved VB/IB + MeshBufferInfo map.', 'Material rows packed into SSBO; lights dual-pack for deferred vs PT.', 'No Vulkan types in scene/ — upload is the boundary.'],
        ),
        page(
            'rt-build',
            'RT build (BLAS/TLAS)',
            'buildBLASTLAS order, instance transforms, material lockstep.',
            files=['ohao/gpu/vulkan/rt_build.cpp', 'ohao/render/rt/rt_acceleration_structure.hpp'],
            design=['Per-mesh BLAS then TLAS instances with world transforms.', 'Invariant: TLAS instance order == material row order (hit shaders index by instanceId).', 'Rebuild on topology change; update transforms when only TRS dirty.'],
        ),
        page(
            'dispatch',
            'Render dispatch',
            'renderDeferred vs RT path, staging readback ring.',
            files=['ohao/gpu/vulkan/render_dispatch.cpp', 'ohao/render/frame/frame_resources.hpp'],
            design=['Branches on RenderMode: deferred graph vs PathTracer::render.', 'MAX_FRAMES_IN_FLIGHT=3; never overwrite in-flight staging.'],
            workflow=['wait fence on ring slot', 'memcpy staging → pixel buffer', 'record cmd, submit, advance frame'],
        ),
        page(
            'layout-meta',
            'Layout contracts',
            'OHAO_ASSERT_GPU_LAYOUT, MaterialGpuPack, push-constant sizes.',
            files=['ohao/gpu/layout_meta.hpp', 'ohao/gpu/vulkan/material.hpp', 'ohao/gpu/vulkan/material.cpp', 'ohao/gpu/vulkan/material_instance.hpp', 'ohao/gpu/vulkan/material_instance.cpp'],
            design=['layout_meta.hpp is the single source of truth for GPU struct sizes/offsets shared with GLSL.', 'Material + MaterialInstance are CPU-side authoring; packed rows feed bindless + PT material SSBO.', 'OHAO_ASSERT_GPU_LAYOUT fails the build if C++ and shader disagree — silent pink is worse.'],
        ),
        page(
            'pipeline-fb',
            'Legacy pipeline & framebuffer',
            'Forward pipeline creation, offscreen FB, shadow resources.',
            files=['ohao/gpu/vulkan/pipeline.cpp', 'ohao/gpu/vulkan/framebuffer.cpp'],
            design=['Forward path remains for simple demos and regression; deferred is the production raster path.', 'Framebuffer helpers allocate offscreen color/depth used by present and readback.'],
        ),
    ],
}
