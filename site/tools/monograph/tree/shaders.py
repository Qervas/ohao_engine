"""Tree units: Shaders."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'shaders',
    "title": 'Shaders',
    "hub": None,
    "children": [
        page(
            'layout',
            'Shader tree layout',
            'core / rt / postprocess / compute / includes / shadow / vfx.',
            files=['shaders/CMakeLists.txt'],
            design=['SPIR-V outputs to build/shaders or bin/shaders; PathTracer searches multiple relative paths.', 'Include path: GL_GOOGLE_include_directive with shaders/ as root.'],
        ),
        page(
            'includes-lighting',
            'Lighting includes',
            'clustered_lights.slang: the falloff, the light loop and the light record.',
            files=['shaders/includes/lighting/clustered_lights.slang', 'shaders/core/deferred_lighting.slang', 'shaders/shared/layout.json'],
            design=['clustered_lights.slang: windowed inverse square, exactly zero past the range (clustered lighting depends on it), the light loop (directLight) through OpenPBR\'s base for deferred and the transparent pass; deferred_lighting.slang adds the split-sum IBL.', 'LightGpu (shared/layout.json): the 64-byte raster light record.'],
        ),
        page(
            'includes-shadow',
            'Shadow sampling in lighting',
            'RT visibility channels, the CSM fallback, the cascade depth write.',
            files=['shaders/core/deferred_lighting.slang', 'shaders/includes/lighting/clustered_lights.slang', 'shaders/shadow/shadow_csm.vert', 'ohao/render/scene/light_table.cpp'],
            design=['RT shadows: eight 8-bit channels (RG32UI, binding 14), params.w names a light\'s; the first eight casters own one, the rest render unshadowed.', 'CSM fallback for directional lights only: cascade by view depth, 3x3 hardware-compare PCF, constant bias.', 'shadow_csm.vert: one vertex stage per cascade, draw record at gl_InstanceIndex.'],
        ),
        page(
            'postprocess-chain',
            'Postprocess shader chain',
            'Fullscreen, bloom, TAA, tonemap, sky, SSR, SSS.',
            files=['shaders/postprocess/fullscreen.vert', 'shaders/postprocess/bloom_threshold.frag', 'shaders/postprocess/bloom_downsample.frag', 'shaders/postprocess/bloom_upsample.frag', 'shaders/postprocess/taa_resolve.frag', 'shaders/postprocess/tonemapping.frag', 'shaders/postprocess/sky.frag', 'shaders/postprocess/ssr.slang', 'shaders/postprocess/sss.slang'],
            design=['fullscreen.vert feeds every post fragment pass.', 'Bloom: threshold → hierarchical down → up; TAA before final tonemap.', 'SSR (half resolution) and SSS (classified tiles) are Slang compute; tonemap upsamples the SSR.'],
            workflow=['threshold', 'downsample mips', 'upsample combine', 'TAA history', 'ACES/tonemap'],
        ),
        page(
            'rt-includes',
            'RT shader includes',
            'pbr_unpack and the NRD frontend used by raygen.',
            files=['shaders/includes/pbr_unpack.glsl', 'shaders/includes/rt/nrd_frontend.glsl'],
            design=['pbr_unpack: material row → albedo/metal/rough/F0 for hit and raygen.', 'nrd_frontend: YCoCg pack + norm hit-dist — wrong pack = magenta REBLUR.'],
        ),
        page(
            'compute',
            'Compute shaders',
            'SSAO, light clustering, capture preview, DLSS display pass, GPU culling and Hi-Z.',
            files=['shaders/compute/ssao.slang', 'shaders/compute/light_cluster.slang', 'shaders/compute/capture_preview.slang', 'shaders/compute/dlss_tonemap.comp', 'shaders/cull/instances.slang', 'shaders/cull/hiz.slang'],
            design=['ssao.slang: half-resolution horizon search and blur for the deferred frame.', 'light_cluster.slang: lights.cluster, every froxel\'s light list (the clustered-lighting unit).','cull/instances.slang + cull/hiz.slang: GpuCulling\'s indirect lists and max-depth pyramid.', 'dlss_tonemap.comp: the DLSS-RR display pass, behind OHAO_DLSS_ENABLED.'],
        ),
        page(
            'core-gbuffer-lighting',
            'Core GBuffer & lighting shaders',
            'gbuffer.vert/frag, deferred_lighting.slang, the eight-target MRT contract.',
            files=['shaders/core/gbuffer.vert', 'shaders/core/gbuffer.frag', 'shaders/includes/scene/gbuffer_body.glsl', 'shaders/core/deferred_lighting.slang'],
            design=['GBuffer MRT, eight targets: position, normal, albedo, velocity (from current/prev clip pos), depth, emission, specular (6), OpenPBR (7).', 'Bindless texture indices via nonuniformEXT; 0xFFFFFFFF = missing map.', 'deferred_lighting.slang: OpenPBR\'s base over the froxel\'s light list (directLight) + split-sum IBL + CSM + optional RT shadow/GI textures.'],
        ),
        page(
            'overlay-gizmo',
            'Overlay gizmo shaders',
            'Editor/debug gizmo vert+frag.',
            files=['shaders/overlay/gizmo.vert', 'shaders/overlay/gizmo.frag'],
        ),
        page(
            'rt-shaders',
            'RT program family',
            'path_tracer.slang, pt_*, shadow_*/gi_* (Slang), NRD compose, SVGF, cinematic.',
            files=['shaders/rt/path_tracer.slang', 'shaders/rt/pt_raygen_realtime.rgen', 'shaders/rt/pt_closesthit.rchit', 'shaders/rt/pt_anyhit.rahit', 'shaders/rt/pt_miss.rmiss', 'shaders/rt/shadow_trace.slang', 'shaders/rt/shadow_filter.slang', 'shaders/rt/gi_trace.slang', 'shaders/rt/gi_denoise.slang', 'shaders/rt/nrd_compose.comp', 'shaders/rt/rt_svgf_temporal.comp', 'shaders/rt/rt_svgf_atrous.comp', 'shaders/rt/cinematic_bloom_extract.comp', 'shaders/rt/cinematic_bloom_blur.comp', 'shaders/rt/cinematic_dof.comp', 'shaders/rt/cinematic_composite.comp'],
            design=['path_tracer.slang = the offline reference (Slang, four entry points, OpenPBR base); pt_raygen_realtime.rgen + pt_closesthit/anyhit/miss = the realtime GLSL tracer.', 'Hybrid (Slang compute, inline ray queries): shadow_* visibility; gi_* one-bounce diffuse GI into deferred.', 'Post-RT: NRD compose unpack, optional SVGF/à-trous, cinematic bloom/DoF.'],
        ),
    ],
}
