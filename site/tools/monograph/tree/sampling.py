"""Tree units: 07 · Sampling."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'sampling',
    "title": '07 · Sampling',
    "hub": 'sampling.html',
    "children": [
        page(
            'sampler-api',
            'Sampler API',
            'sampler_api.glsl dispatch, specialization constant.',
            files=['shaders/includes/rt/sampler_api.glsl', 'ohao/render/rt/sampler_types.hpp'],
            design=['Unified next1D/next2D API; backend selected by profile (PCG vs Sobol).', 'sampler_types.hpp mirrors host enum for specialization constants.'],
        ),
        page(
            'sobol',
            'Sobol + Owen',
            'Offline low-discrepancy stream.',
            files=['shaders/includes/rt/sampler_sobol.glsl', 'shaders/includes/rt/sampler_sobol_tables.glsl', 'ohao/render/rt/sobol_generator.cpp', 'ohao/render/rt/owen_scramble.cpp'],
            design=['Sobol sequences + Owen scrambling for offline convergence.', 'Tables generated/loaded on host; GPU samples by dimension + pixel/sample index.'],
        ),
        page(
            'pcg',
            'PCG realtime',
            'Hash-based sampler for interactive path.',
            files=['shaders/includes/rt/sampler_pcg.glsl'],
            design=['Cheap hash RNG for interactive path tracing; quality traded for speed vs Sobol.'],
        ),
        page(
            'mis',
            'MIS heuristics',
            'Balance and power in mis.glsl.',
            files=['shaders/includes/rt/mis.glsl'],
            design=['Balance heuristic default; power heuristic optional for NEE vs BRDF.', 'Weights use pdf_light and pdf_bsdf at the same sample — unit mismatch = bias.'],
        ),
        page(
            'env-cdf',
            'Environment CDF',
            'CPU build + GPU sampleEnvMap/pdfEnvMap.',
            files=['ohao/render/rt/env_cdf.cpp', 'ohao/render/rt/env_cdf.hpp', 'shaders/includes/rt/env_sampling.glsl'],
            design=['CPU builds marginal + conditional CDFs from HDR luminance.', 'GPU sampleEnvMap / pdfEnvMap for importance-sampled sky lighting in miss/NEE.'],
        ),
    ],
}
