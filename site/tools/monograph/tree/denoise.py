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
            'DenoiseMode enum + rt_meta\'s denoise policy functions.',
            files=['ohao/render/rt/denoise/denoise_types.hpp', 'ohao/render/rt/rt_meta.hpp', 'ohao/render/rt/denoiser.hpp'],
            design=['DenoiseMode: None | OIDN | NRD | Atrous | DLSSRR (and related).', 'denoiseNeeds* functions say which AOV guides each backend needs (albedo, normal, hit-dist, motion); NRD is the realtime default.', 'CLI: --denoise=dlssrr | nrd | oidn | atrous | none.'],
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
            'Pack YCoCg, dispatch, compose with the emission guide into the presented frame.',
            files=['ohao/render/rt/denoise/nrd_denoise.hpp', 'ohao/render/rt/denoise/nrd_compose.hpp', 'shaders/includes/rt/nrd_frontend.glsl', 'shaders/rt/nrd_compose.comp'],
            design=['REBLUR expects IN_DIFF/SPEC as YCoCg + normalized hit-distance — not linear RGB.', 'nrd_frontend.glsl: nrdPackRadianceHitDist / nrdYCoCgToLinear.', 'nrd_compose.comp: composed = diff x diffAlbedo + spec + emission (binding 5), and the frame\'s RGBA8 output (binding 6) with the tracer\'s curve; a window presents the denoised frame.', 'NRD is the realtime default without DLSS-RR (non-NVIDIA, or built without the SDK).'],
            workflow=['pack AOVs in path_tracer.slang\'s raygen', 'NRD dispatch', 'nrd_compose.comp unpack + emission', 'present'],
            why='Realtime temporal denoise that reuses engine AOVs already required for hybrid GI.',
        ),
        page(
            'dlss',
            'DLSS Ray Reconstruction',
            'NGX dlssd init, guides, the display tonemap; the realtime default on NVIDIA.',
            files=['ohao/render/rt/denoise/dlss_rr.hpp', 'ohao/render/rt/denoise/dlss_rr.cpp', 'shaders/compute/dlss_tonemap.comp', 'tools/fetch_dlss.py', 'external/cmake/dlss.cmake'],
            design=['NVIDIA DLSS Ray Reconstruction via NGX; tools/fetch_dlss.py fetches the pinned SDK (v310.9.16, SHA-256 per file) into git-ignored external/DLSS; the static nvsdk_ngx_s.lib links, nvngx_dlssd.dll is copied next to the binaries.', 'rtDefaults picks DLSSRR for the realtime profile when built with the SDK on an NVIDIA device (0x10DE); captures default to no denoiser.', 'NGX logs go through OHAO_LOG (Debug), so stdout stays the protocol\'s.', 'dlss_tonemap.comp: ACES(x * 0.5)^(1/2.2) into the RGBA8 output, one descriptor set per frame in flight.'],
        ),
        page(
            'atrous',
            'À-trous / SVGF-style',
            'Internal spatial/temporal denoise compute.',
            files=['ohao/render/rt/denoise/atrous_denoise.hpp', 'shaders/compute/denoise_atrous.comp', 'shaders/rt/rt_atrous.comp', 'shaders/rt/rt_svgf_temporal.comp', 'shaders/rt/rt_svgf_atrous.comp'],
            design=['In-engine fallback when NRD/DLSS unavailable: edge-aware à-trous + optional SVGF temporal.', 'Does not match NRD quality but has zero external dependency.'],
        ),
    ],
}
