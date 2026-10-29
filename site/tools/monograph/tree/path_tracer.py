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
            files=['ohao/render/rt/path_tracer.hpp', 'ohao/render/rt/path_tracer.cpp', 'ohao/render/rt/path_tracer_pipeline.cpp', 'ohao/render/rt/path_tracer_descriptors.cpp', 'ohao/render/rt/path_tracer_images.cpp', 'ohao/render/rt/path_tracer_render.cpp', 'ohao/render/rt/rt_module.hpp', 'ohao/render/rt/rt_render_pipeline.hpp', 'ohao/render/render_module.hpp'],
            design=['PathTracer owns accum (RGBA32F), output (RGBA8), AOVs, SBT, and optional denoise backends.', 'Split TUs: pipeline create, descriptor update, image alloc/resize, render record.', 'rt_render_pipeline.hpp bridges VulkanRenderer mode dispatch into PathTracer::render.', 'ODR rule: NRD/DLSS members stay unconditional in the header so ohao_gpu_vulkan and ohao_renderer agree on layout.'],
            workflow=['init(device, phys, w, h)', 'setMaterialAlbedos / lights / env CDF', 'render(cmd, accel, view, proj, …)', 'resetAccumulation on camera move', 'resize reallocates images + descriptor writes'],
            why='Full-frame GI without a separate light bake; progressive samples until denoise or offline converge.',
        ),
        page(
            'profiles',
            'Profiles & meta',
            'RTRenderSettings, Realtime vs Offline traits.',
            files=['ohao/render/rt/rt_settings.hpp', 'ohao/render/rt/rt_meta.hpp', 'ohao/render/rt/rt_profile_renderer.hpp'],
            design=['kRealtimeRTSettings vs kOfflineRTSettings: spp, max bounce, sampler type, denoise defaults.', 'rt_meta traits drive compile-time and runtime feature gates (ReSTIR planes, NRD packing).', 'Never construct both realtime and offline PathTracer at 4K without explicit opt-in — VRAM doubles.'],
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
            'The OpenPBR reference: NEE + BSDF sampling under MIS, AOV writes.',
            files=['shaders/rt/path_tracer.slang', 'shaders/material/openpbr.slang', 'shaders/rt/pt_raygen_realtime.rgen'],
            design=['path_tracer.slang is the offline reference: one path a sample estimating OpenPBR\'s base BSDF (material/openpbr.slang).', 'Every vertex: NEE to one uniformly chosen light (sphere by its cone, rectangle by area, sun as a delta) and to the environment CDF, each weighed against BSDF sampling by the power heuristic.', 'A bounce ray that meets a light\'s emitter mesh is weighed against light sampling by the light index in the emitter instance; lights without a mesh take full weight.', 'Russian roulette from the third vertex; 32 bounces by default for the offline profile.', 'AOV writes: albedo, normal, depth, roughness, diffuse/specular radiance split by the first lobe, motion vectors for denoisers.', 'The realtime profile keeps the GLSL raygen (pt_raygen_realtime.rgen). Hub monograph has the line-anchored NEE walk (L241+).'],
            workflow=['Camera ray through a jittered pixel point', 'Closest hit resolves OpenPBR inputs into the payload', 'NEE: one light + one environment direction, shadow rays, MIS', 'BSDF sample next direction', 'Russian roulette / max bounce', 'Accumulate into HDR buffer + AOV images'],
            why='One estimator for the reference image; the profile picks the shader set and knobs without forking the host API.',
        ),
        page(
            'hit-miss',
            'Hit & miss shaders',
            'closesthit materials, anyhit alpha, miss env.',
            files=['shaders/rt/path_tracer.slang', 'shaders/rt/pt_closesthit.rchit', 'shaders/rt/pt_anyhit.rahit', 'shaders/rt/pt_miss.rmiss'],
            design=['Offline: path_tracer.slang\'s closestHit/anyHit/miss entries; realtime: the GLSL pt_* stages.', 'closesthit: barycentrics → material rows and textures → OpenPBR inputs into payload.', 'anyhit: alpha-tested geometry; ignoreIntersectionEXT when cutout fails.', 'miss: environment map / constant sky contribution into path radiance.'],
        ),
        page(
            'bindings',
            'Descriptor map 0–38',
            'Full set-0 layout including ReSTIR 29–34 and DLSS hit-dist 35.',
            files=['ohao/render/rt/path_tracer_descriptors.cpp', 'site/assets/plates/pt_bindings.svg'],
            design=['Binding 0: TLAS. 1–2: accum + output. 3: materials. 11: GPULight SSBO.', '17–18: env CDF. 19–26: motion, depth, roughness, split radiance/albedo AOVs.', '29–34: ReSTIR reservoir ping-pong images (realtime).', '35: DLSS hit-distance guide when DenoiseMode::DLSSRR.', '36–37: RenderScene instances and material refs. 38: OpenPBR energy tables (raygen).', 'Descriptor writes must match GLSL layout(set=0, binding=N) exactly — silent black if wrong.'],
        ),
        page(
            'restir',
            'ReSTIR GI reservoirs',
            'Ping-pong reservoir planes on bindings 29–34.',
            files=['ohao/render/rt/path_tracer_descriptors.cpp'],
            design=['Realtime GI resampling stores light samples in reservoir images for temporal reuse.', 'Planes live in set-0 as storage images — same descriptor set as the path tracer.', 'The offline reference (path_tracer.slang) does not read the reservoirs.'],
        ),
    ],
}
