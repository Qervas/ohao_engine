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
            'core / rt / postprocess / compute / includes / shadow / particles.',
            files=['shaders/CMakeLists.txt'],
            design=['SPIR-V outputs to build/shaders or bin/shaders; PathTracer searches multiple relative paths.', 'Active sources only — shaders/_disabled is archival and not in the build graph.', 'Include path: GL_GOOGLE_include_directive with shaders/ as root.'],
        ),
        page(
            'includes-common',
            'Common includes',
            'math, color, encoding, reconstruction, constants, types.',
            files=['shaders/includes/common/math.glsl', 'shaders/includes/common/color.glsl', 'shaders/includes/common/encoding.glsl', 'shaders/includes/common/reconstruction.glsl', 'shaders/includes/common/constants.glsl', 'shaders/includes/common/types.glsl', 'shaders/includes/common/common.glsl', 'shaders/includes/noise.glsl'],
            design=['Shared helpers for both raster and RT — keep one definition of luminance, safe normalize, etc.', 'encoding.glsl: octahedral / pack conventions used by GBuffer and AOVs.', 'noise.glsl: procedural hash for particles, cloud density.'],
        ),
        page(
            'includes-lighting',
            'Lighting includes',
            'IBL, attenuation, light types, phase, blinn_phong.',
            files=['shaders/includes/lighting/ibl.glsl', 'shaders/includes/lighting/light_attenuation.glsl', 'shaders/includes/lighting/light_types.glsl', 'shaders/includes/lighting/phase.glsl', 'shaders/includes/lighting/blinn_phong.glsl'],
            design=['ibl.glsl samples prefiltered specular + irradiance + BRDF LUT for deferred and hybrid.', 'light_types.glsl mirrors CPU LightData / GPULight packing conventions.', 'phase.glsl for volume/cloud scattering lobes.'],
        ),
        page(
            'includes-shadow',
            'Shadow includes',
            'CSM sampling, PCF, shadow types.',
            files=['shaders/includes/shadow/shadow_csm.glsl', 'shaders/includes/shadow/shadow_pcf.glsl', 'shaders/includes/shadow/shadow_types.glsl', 'shaders/shadow/shadow_depth.vert', 'shaders/shadow/shadow_depth.frag', 'shaders/shadow/shadow_csm.vert'],
            design=['shadow_depth.* = single-map depth; shadow_csm.vert = cascade-aware depth write.', 'PCF/CSM sample helpers shared by deferred_lighting.frag.'],
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
            'pbr_unpack, rt_masks, NRD frontend used by raygen.',
            files=['shaders/rt/includes/pbr_unpack.glsl', 'shaders/rt/includes/rt_masks.glsl', 'shaders/includes/rt/nrd_frontend.glsl', 'shaders/includes/pbr_unpack.glsl'],
            design=['pbr_unpack: material row → albedo/metal/rough/F0 for hit and raygen.', 'rt_masks: visibility/anyhit mask bits.', 'nrd_frontend: YCoCg pack + norm hit-dist — wrong pack = magenta REBLUR.'],
        ),
        page(
            'compute',
            'Compute shaders',
            'SSAO, capture preview, DLSS display pass, GPU culling and Hi-Z.',
            files=['shaders/compute/ssao.slang', 'shaders/compute/capture_preview.slang', 'shaders/compute/dlss_tonemap.comp', 'shaders/cull/instances.slang', 'shaders/cull/hiz.slang'],
            design=['ssao.slang: half-resolution horizon search and blur for the deferred frame.', 'cull/instances.slang + cull/hiz.slang: GpuCulling\'s indirect lists and max-depth pyramid.', 'dlss_tonemap.comp: the DLSS-RR display pass, behind OHAO_DLSS_ENABLED.'],
        ),
        page(
            'core-gbuffer-lighting',
            'Core GBuffer & lighting shaders',
            'gbuffer.vert/frag, deferred_lighting.frag MRT contract.',
            files=['shaders/core/gbuffer.vert', 'shaders/core/gbuffer.frag', 'shaders/core/deferred_lighting.frag'],
            design=['GBuffer MRT: albedo, normal, material, velocity (from current/prev clip pos), depth.', 'Bindless texture indices via nonuniformEXT; 0xFFFFFFFF = missing map.', 'deferred_lighting.frag: light loop + IBL + CSM + optional RT shadow/GI textures.'],
        ),
        page(
            'particles-sh',
            'Particle shaders',
            'Emit, update, render.',
            files=['shaders/particles/particle_emit.comp', 'shaders/particles/particle_update.comp', 'shaders/particles/particle_render.vert', 'shaders/particles/particle_render.frag'],
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
            'pt_*, shadow_*/gi_* (Slang), NRD compose, SVGF, cinematic.',
            files=['shaders/rt/pt_raygen.rgen', 'shaders/rt/pt_raygen_realtime.rgen', 'shaders/rt/pt_raygen_offline.rgen', 'shaders/rt/pt_closesthit.rchit', 'shaders/rt/pt_anyhit.rahit', 'shaders/rt/pt_miss.rmiss', 'shaders/rt/shadow_trace.slang', 'shaders/rt/shadow_filter.slang', 'shaders/rt/gi_trace.slang', 'shaders/rt/gi_denoise.slang', 'shaders/rt/nrd_compose.comp', 'shaders/rt/rt_svgf_temporal.comp', 'shaders/rt/rt_svgf_atrous.comp', 'shaders/rt/cinematic_bloom_extract.comp', 'shaders/rt/cinematic_bloom_blur.comp', 'shaders/rt/cinematic_dof.comp', 'shaders/rt/cinematic_composite.comp'],
            design=['pt_raygen.rgen = reference brute force; realtime/offline variants add NEE/MIS/AOV packing.', 'Hybrid (Slang compute, inline ray queries): shadow_* visibility; gi_* one-bounce diffuse GI into deferred.', 'Post-RT: NRD compose unpack, optional SVGF/à-trous, cinematic bloom/DoF.'],
        ),
    ],
}
