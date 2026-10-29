"""Tree units: 05 · Deferred."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'deferred',
    "title": '05 · Deferred',
    "hub": 'deferred.html',
    "children": [
        page(
            'orchestrator',
            'DeferredRenderer orchestrator',
            'Pass ownership, resize, setScene, graph execute.',
            files=['ohao/render/deferred/deferred_renderer.hpp', 'ohao/render/deferred/deferred_renderer.cpp'],
            design=['Owns GBuffer, CSM, lighting, SSAO, SSR/SSS, sky, post, gizmo, optional RT shadow/GI techniques.', 'render(cmd, frameIndex) is the per-frame entry; RenderGraph inserts barriers between passes.', 'setScene + geometry buffers come from VulkanRenderer after upload.', 'Hybrid: RTShadowTechnique / RTGITechnique inject visibility and bounce GI into lighting.'],
            workflow=['Culling begins', 'CSM', 'GBuffer (early, Hi-Z, late)', 'Light clusters', 'SSAO','Lighting (+ RT)', 'Sky', 'Post (bloom/TAA/tonemap)', 'Gizmo'],
        ),
        page(
            'pass-base',
            'RenderPassBase',
            'Shared init/cleanup/shader path helpers for passes.',
            files=['ohao/render/deferred/render_pass_base.hpp', 'ohao/render/deferred/render_pass_base.cpp'],
            design=['Common Vk pipeline/layout/shader load so each pass only declares attachments and record().'],
        ),
        page(
            'gbuffer',
            'GBuffer pass',
            'GPU-culled early/late passes, MRT packing, bindless, velocity.',
            files=['ohao/render/deferred/gbuffer_pass.hpp', 'ohao/render/deferred/gbuffer_pass.cpp', 'shaders/core/gbuffer.vert', 'shaders/core/gbuffer.frag'],
            design=['Five timed parts: cull.instances, gbuffer.early, hiz.build, cull.late, gbuffer.late; indirect draws, one-sided materials back-face culled.', 'Vertex: the draw record at firstInstance gives the instance and material slots; world pos, normal, tangent, dual UV, current+prev clip for velocity.', 'Fragment MRT, eight targets: position, packed normal, albedo (base_weight x base_color), motion vectors, depth, emission (emission_color x emission_luminance), specular (specular_color, specular_weight / 1.5), OpenPBR (base_diffuse_roughness, (specular_ior - 1) / 2.55); bindless textures via nonuniformEXT, indices from the material row.'],
        ),
        page(
            'csm',
            'CSM shadows',
            'Cascade split, depth array, unjittered camera.',
            files=['ohao/render/deferred/csm_pass.hpp', 'shaders/shadow/shadow_csm.vert'],
            design=['Cascaded shadow maps into depth array; splits from camera frustum.', 'Each cascade GPU-culled to its side planes, drawn indirectly.', 'Unjittered camera for stable cascades when TAA is on.'],
        ),
        page(
            'lighting',
            'Deferred lighting',
            'The froxel\'s light loop through OpenPBR\'s base, split-sum IBL, SSAO, shadows.',
            files=['ohao/render/deferred/deferred_lighting_pass.hpp', 'ohao/render/deferred/deferred_lighting_pass.cpp', 'shaders/core/deferred_lighting.slang', 'shaders/includes/lighting/clustered_lights.slang', 'shaders/material/openpbr.slang'],
            design=['Fullscreen Slang pass reads world position from GBuffer0 (no depth reconstruction) and rebuilds OpenPBR\'s inputs from the eight targets.', 'directLight (clustered_lights.slang) sums the pixel\'s froxel\'s lights through OpenPBRBase.evalLobes, shadowed by RT visibility or the CSM; the split-sum weight is OpenPBRBase.specularAlbedo from OpenPBR\'s energy tables (binding 20), the diffuse fill diffuseAlbedo; SSAO on the ambient terms; RT GI.'],
        ),
        page(
            'clustered-lighting',
            'Clustered lighting',
            'Froxel light lists, built every frame, exact against summing every light.',
            files=['ohao/render/deferred/light_clusters.hpp', 'ohao/render/deferred/light_clusters.cpp', 'shaders/compute/light_cluster.slang', 'shaders/includes/lighting/clustered_lights.slang', 'tests/renderer/light_cluster_gpu_test.cpp'],
            design=['16x9 tiles x 24 exponential slices (0.1 m to the far plane) = 3,456 froxels; lists of at most 256 light indices in light order.', 'lights.cluster: one 64-thread workgroup per froxel, sphere vs conservative view-space box, ordered groupshared compaction, no atomics across workgroups.', 'Light table: a LightGpu storage buffer per frame slot, rewritten only when the version changes; overflow into render.stats lightsOverflow.'],
            workflow=['setLights', 'lights.cluster', 'lighting', 'collect overflow'],
        ),
        page(
            'ssao',
            'SSAO',
            'Half-resolution horizon AO, blurred, upsampled into the ambient terms.',
            files=['ohao/render/deferred/ssao_pass.hpp', 'ohao/render/deferred/ssao_pass.cpp', 'shaders/compute/ssao.slang'],
            design=['Horizon search in the tangent plane from G-buffer world position and normal, at half resolution, 4x4 seeded noise in push constants.', '4x4 depth-aware blur; lighting upsamples (binding 10, flag bit 1) and multiplies only ambient and IBL.'],
        ),
        page(
            'ssr-sss',
            'SSR & SSS',
            'Half-resolution reflections; subsurface blur on classified tiles.',
            files=['ohao/render/deferred/ssr_pass.hpp', 'ohao/render/deferred/sss_pass.hpp', 'shaders/postprocess/ssr.slang', 'shaders/postprocess/sss.slang'],
            design=['SSR: 64-step world-space march at half resolution; tonemap upsamples it against depth.', 'SSS: separable blur in place on the lit image over 16x16 tiles the G-buffer flags for materials with subsurface > 0, indirect dispatches.'],
        ),
        page(
            'sky',
            'Sky pass',
            'HDR sky / atmosphere composite into lighting output.',
            files=['ohao/render/deferred/sky_pass.hpp', 'shaders/postprocess/sky.frag'],
            design=['Composites HDR environment/atmosphere where GBuffer depth is sky.'],
        ),
        page(
            'post',
            'Post stack',
            'Bloom, TAA, tonemap orchestration.',
            files=['ohao/render/deferred/post_processing_pipeline.hpp', 'ohao/render/deferred/bloom_pass.hpp', 'ohao/render/deferred/taa_pass.hpp', 'shaders/postprocess/tonemapping.frag', 'shaders/postprocess/taa_resolve.frag'],
            design=['PostProcessingPipeline owns order; bloom and TAA are separate passes sharing history/HDR targets.'],
            workflow=['executeSSAO early', 'lighting', 'bloom threshold/down/up', 'TAA resolve', 'tonemap ACES/…'],
        ),
        page(
            'gizmo',
            'Gizmo pass',
            'Debug/editor overlay meshes.',
            files=['ohao/render/deferred/gizmo_pass.hpp', 'ohao/render/gizmo/gizmo_meshes.hpp', 'shaders/overlay/gizmo.vert'],
            design=['Line/solid debug meshes drawn after tonemap path for selection and force viz.'],
        ),
    ],
}
