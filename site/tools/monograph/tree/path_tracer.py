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
            'Stages A/B/C, NEE, dual-lobe, AOV writes.',
            files=['shaders/rt/pt_raygen.rgen', 'shaders/rt/pt_raygen_realtime.rgen', 'shaders/rt/pt_raygen_offline.rgen'],
            design=['pt_raygen.rgen is the brute-force reference: camera ray → bounce → light hit, no NEE.', 'Realtime/offline variants add NEE (next-event estimation), dual-lobe BRDF sampling, and MIS.', 'NEE sphere sample: sample light disk by solid angle, shadow ray with d=lightDist (not radius) for Tmin/Tmax.', 'AOV writes: albedo, normal, depth, roughness, diffuse/specular radiance, motion vectors for denoisers.', 'NRD path packs radiance as YCoCg + normalized hit-distance before REBLUR (see nrd_frontend.glsl).', 'Hub monograph has the line-anchored NEE walk (L305+) with GLSL and why-notes.'],
            workflow=['Primary ray from camera UBO', 'Closest-hit payload: throughput, flags', 'NEE sample + shadow ray (optional)', 'BRDF sample next direction (dual lobe)', 'Russian roulette / max bounce', 'Accumulate into HDR buffer + AOV images'],
            why='One integrator family; profile swaps spp/sampler without forking the host API.',
        ),
        page(
            'hit-miss',
            'Hit & miss shaders',
            'closesthit materials, anyhit alpha, miss env.',
            files=['shaders/rt/pt_closesthit.rchit', 'shaders/rt/pt_anyhit.rahit', 'shaders/rt/pt_miss.rmiss'],
            design=['closesthit: barycentrics → material row → PBR params into payload.', 'anyhit: alpha-tested geometry; ignoreIntersectionEXT when cutout fails.', 'miss: environment map / constant sky contribution into path radiance.'],
        ),
        page(
            'bindings',
            'Descriptor map 0–35',
            'Full set-0 layout including ReSTIR 29–34 and DLSS hit-dist 35.',
            files=['ohao/render/rt/path_tracer_descriptors.cpp', 'site/assets/plates/pt_bindings.svg'],
            design=['Binding 0: TLAS. 1–2: accum + output. 3: materials. 11: GPULight SSBO.', '17–18: env CDF. 19–26: motion, depth, roughness, split radiance/albedo AOVs.', '29–34: ReSTIR reservoir ping-pong images (realtime).', '35: DLSS hit-distance guide when DenoiseMode::DLSSRR.', 'Descriptor writes must match GLSL layout(set=0, binding=N) exactly — silent black if wrong.'],
        ),
        page(
            'restir',
            'ReSTIR GI reservoirs',
            'Ping-pong reservoir planes on bindings 29–34.',
            files=['ohao/render/rt/path_tracer_descriptors.cpp'],
            design=['Realtime GI resampling stores light samples in reservoir images for temporal reuse.', 'Planes live in set-0 as storage images — same descriptor set as the path tracer.', 'Offline profile may skip ReSTIR and rely on spp + Sobol instead.'],
        ),
    ],
}
