"""Tree units: 06 · Path tracer."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'path-tracer',
    "title": '06 · Path tracer',
    "hub": 'path-tracer.html',
    "children": [
        page(
            'host',
            'Host lifecycle',
            'ensureRTRenderer, images, pipeline, SBT, descriptors.',
            files=['ohao/render/rt/path_tracer.hpp', 'ohao/render/rt/path_tracer.cpp', 'ohao/render/rt/path_tracer_pipeline.cpp', 'ohao/render/rt/path_tracer_descriptors.cpp', 'ohao/render/rt/path_tracer_images.cpp', 'ohao/render/rt/path_tracer_render.cpp', 'ohao/render/rt/rt_module.hpp', 'ohao/render/render_module.hpp'],
            design=['PathTracer owns accum (RGBA32F), output (RGBA8), AOVs, SBT, and optional denoise backends.', 'Split TUs: pipeline create, descriptor update, image alloc/resize, render record.', 'VulkanRenderer takes the mode\'s defaults (rtDefaults) and runs prepareRTSceneForFrame / renderRTPipeline with them into PathTracer::render.', 'Two clocks: m_historyFrameCount (the denoisers\' history, reset only by resetAccumulation) and m_accumFrames (the running mean, restarted by a moved view).', 'ODR rule: NRD/DLSS members stay unconditional in the header so ohao_gpu_vulkan and ohao_renderer agree on layout.'],
            workflow=['init(device, phys, w, h)', 'setMaterialAlbedos / lights / env CDF', 'render(cmd, accel, view, proj, …)', 'resetAccumulation on camera move', 'resize reallocates images + descriptor writes'],
            why='Full-frame GI without a separate light bake; progressive samples until denoise or offline converge.',
        ),
        page(
            'profiles',
            'Profiles & meta',
            'RTRenderSettings, Realtime vs Offline traits.',
            files=['ohao/render/rt/rt_settings.hpp', 'ohao/render/rt/rt_meta.hpp', 'ohao/render/rt/rt_profile_renderer.hpp'],
            design=['kRealtimeRTSettings vs kOfflineRTSettings: max bounces, accumulation, AOVs, denoiser (NRD realtime by default); both profiles run rt/path_tracer.slang.', 'rt_meta.hpp: constexpr policy functions (denoiseNeeds*, denoiseIsRealtimeCapable, tracerAccumulates, tracerAccumulatesAOVs, applyDenoisePolicy) and the technique concepts.', 'Never construct both realtime and offline PathTracer at 4K without explicit opt-in — VRAM doubles.'],
        ),
        page(
            'as',
            'Acceleration structures',
            'BLAS/TLAS API, rebuild vs update.',
            files=['ohao/render/rt/rt_acceleration_structure.hpp', 'ohao/render/rt/rt_acceleration_structure.cpp'],
        ),
        page(
            'raygen',
            'Raygen integrator',
            'The OpenPBR path tracer: NEE + BSDF sampling under MIS, AOV writes.',
            files=['shaders/rt/path_tracer.slang', 'shaders/material/openpbr.slang'],
            design=['path_tracer.slang is the path tracer of both RT profiles: one path a sample estimating OpenPBR\'s base BSDF (material/openpbr.slang).', 'Every vertex: NEE to one uniformly chosen light (sphere by its cone, rectangle by area, sun as a delta) and to the environment CDF, each weighed against BSDF sampling by the power heuristic.', 'A bounce ray that meets a light\'s emitter mesh is weighed against light sampling by the light index in the emitter instance; lights without a mesh take full weight.', 'Russian roulette from the third vertex; 32 bounces by default for the offline profile; the firefly clamp only when the trace does not accumulate.', 'AOV writes: albedo, normal, depth, roughness, diffuse/specular radiance split by the first lobe, motion vectors for denoisers.', 'pc.control.y: the frames in the running mean (0 when a temporal denoiser takes the frame\'s sample alone); pc.control.z: last frame\'s view gives motion vectors. Hub monograph has the line-anchored NEE walk.'],
            workflow=['Camera ray through a jittered pixel point', 'Closest hit resolves OpenPBR inputs into the payload', 'NEE: one light + one environment direction, shadow rays, MIS', 'BSDF sample next direction', 'Russian roulette / max bounce', 'Accumulate into HDR buffer + AOV images'],
            why='One estimator for the reference image; the profile picks the shader set and knobs without forking the host API.',
        ),
        page(
            'hit-miss',
            'Hit & miss shaders',
            'closesthit materials, anyhit alpha, miss env.',
            files=['shaders/rt/path_tracer.slang'],
            design=['path_tracer.slang\'s closestHit/anyHit/miss entries, for both profiles.', 'closesthit: barycentrics → material rows and textures → OpenPBR inputs into payload.', 'anyhit: alpha-tested geometry; ignoreIntersectionEXT when cutout fails.', 'miss: environment map / constant sky contribution into path radiance.'],
        ),
        page(
            'bindings',
            'Descriptor map 0–38',
            'Set-0 layout: TLAS, images, AOVs and guides, scene tables, OpenPBR tables.',
            files=['ohao/render/rt/path_tracer_descriptors.cpp', 'site/assets/plates/pt_bindings.svg'],
            design=['Binding 0: TLAS. 1–2: accum + output. 3: materials. 11: GPULight SSBO.', '17–18: env CDF. 19–26: motion, depth, roughness, split radiance/albedo AOVs.', '36–37: RenderScene instances and material refs. 38: OpenPBR energy tables (raygen).', 'Bindings 13–16 and 27–35 are unused; 13 storage images in all.', 'Descriptor writes must match the Slang [[vk::binding]] numbers exactly — silent black if wrong.'],
        ),
    ],
}
