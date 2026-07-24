"""Tree units: 09 · Denoise."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'denoise',
    "title": '09 · Denoise',
    "hub": 'denoise.html',
    "children": [
        page(
            'types',
            'Denoise modes & traits',
            'DenoiseMode enum + DenoiseModeTraits resource needs.',
            files=['ohao/render/rt/denoise/denoise_types.hpp', 'ohao/render/rt/rt_meta.hpp', 'ohao/render/rt/denoiser.hpp'],
            design=['DenoiseMode: None | OIDN | NRD | Atrous | DLSSRR (and related).', 'Traits declare which AOV guides each backend needs (albedo, normal, hit-dist, motion).', 'CLI: --denoise=dlssrr | nrd | oidn | atrous | none.'],
        ),
        page(
            'oidn',
            'OIDN',
            'Offline guided denoise path.',
            files=['ohao/render/rt/denoise/oidn_denoise.hpp', 'ohao/render/rt/denoise/oidn_denoise.cpp'],
            design=['Intel Open Image Denoise for offline/reference — quality over latency.', 'Typically runs after enough spp accumulate; guided by albedo/normal when available.'],
        ),
        page(
            'nrd',
            'NRD REBLUR + compose',
            'Pack YCoCg, dispatch, unpack compose, cinematic post.',
            files=['ohao/render/rt/denoise/nrd_denoise.hpp', 'ohao/render/rt/denoise/nrd_compose.hpp', 'ohao/render/rt/denoise/nrd_cinematic.hpp', 'shaders/includes/rt/nrd_frontend.glsl', 'shaders/rt/nrd_compose.comp'],
            design=['REBLUR expects IN_DIFF/SPEC as YCoCg + normalized hit-distance — not linear RGB.', 'nrd_frontend.glsl: nrdPackRadianceHitDist / nrdYCoCgToLinear.', 'Compose unpacks after REBLUR; wrong pack = magenta or washed output (classic bug).', 'NrdCinematicPost optional bloom/DoF after stable denoised HDR.'],
            workflow=['pack AOVs in raygen', 'NRD dispatch', 'nrd_compose.comp unpack', 'optional cinematic'],
            why='Realtime temporal denoise that reuses engine AOVs already required for hybrid GI.',
        ),
        page(
            'dlss',
            'DLSS Ray Reconstruction',
            'NGX dlssd init, guides, binding 35 hit-dist.',
            files=['ohao/render/rt/denoise/dlss_rr.hpp', 'ohao/render/rt/denoise/dlss_rr.cpp', 'shaders/compute/dlss_tonemap.comp'],
            design=['NVIDIA DLSS Ray Reconstruction via NGX; needs LD_LIBRARY_PATH to DLSS .so.', 'Guides: color, depth, motion, normal-roughness, specular hit-distance (binding 35).', 'dlss_tonemap.comp prepares display path after RR.', 'interactive --denoise=dlssrr is the primary dogfood path.'],
        ),
        page(
            'atrous',
            'À-trous / SVGF-style',
            'Internal spatial/temporal denoise compute.',
            files=['ohao/render/rt/denoise/atrous_denoise.hpp', 'shaders/compute/denoise_atrous.comp', 'shaders/rt/rt_atrous.comp', 'shaders/rt/rt_svgf_temporal.comp', 'shaders/rt/rt_svgf_atrous.comp'],
            design=['In-engine fallback when NRD/DLSS unavailable: edge-aware à-trous + optional SVGF temporal.', 'Does not match NRD quality but has zero external dependency.'],
        ),
        page(
            'cinematic',
            'Cinematic RT post',
            'Bloom extract/blur, DoF, composite after NRD.',
            files=['shaders/rt/cinematic_bloom_extract.comp', 'shaders/rt/cinematic_bloom_blur.comp', 'shaders/rt/cinematic_dof.comp', 'shaders/rt/cinematic_composite.comp'],
            design=['Runs after denoise so bloom/DoF do not fight temporal filters.', 'Extract → blur mips → composite with optional DoF CoC.'],
        ),
    ],
}
