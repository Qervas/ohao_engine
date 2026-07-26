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
            workflow=['CSM', 'GBuffer', 'SSAO', 'Lighting (+ RT)', 'Sky', 'Post (bloom/TAA/tonemap)', 'Gizmo'],
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
            'MRT packing, bindless, velocity, push constants.',
            files=['ohao/render/deferred/gbuffer_pass.hpp', 'shaders/core/gbuffer.vert', 'shaders/core/gbuffer.frag'],
            design=['Vertex: world pos, normal, tangent, dual UV, current+prev clip for velocity.', 'Fragment MRT: albedo, packed normal, material params, motion vectors.', 'Bindless textures via nonuniformEXT; material push constants carry indices.'],
        ),
        page(
            'csm',
            'CSM shadows',
            'Cascade split, depth array, unjittered camera.',
            files=['ohao/render/deferred/csm_pass.hpp', 'shaders/shadow/shadow_csm.vert', 'shaders/includes/shadow/shadow_csm.glsl'],
            design=['Cascaded shadow maps into depth array; splits from camera frustum.', 'Unjittered camera for stable cascades when TAA is on.'],
        ),
        page(
            'lighting',
            'Deferred lighting',
            'Light loop, IBL, SSAO bind, BRDF eval.',
            files=['ohao/render/deferred/deferred_lighting_pass.hpp', 'shaders/core/deferred_lighting.frag', 'shaders/includes/lighting/ibl.glsl'],
            design=['Fullscreen pass reconstructs world pos from depth + inv VP.', 'Evaluates GGX + IBL + CSM; multiplies SSAO; optional RT shadow/GI textures.'],
        ),
        page(
            'ssao',
            'SSAO',
            'Screen-space AO pass + compute path.',
            files=['ohao/render/deferred/ssao_pass.hpp', 'shaders/compute/ssao.comp'],
            design=['Hemisphere samples in view space; blurred AO bound into lighting.'],
        ),
        page(
            'ssr-sss',
            'SSR & SSS',
            'Experimental reflections and subsurface blur.',
            files=['ohao/render/deferred/ssr_pass.hpp', 'ohao/render/deferred/sss_pass.hpp', 'shaders/postprocess/ssr.comp', 'shaders/postprocess/sss_blur.comp'],
            design=['SSR ray-marches HiZ for glossy reflections; SSS blur for skin-like materials — quality flags on hub.'],
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
