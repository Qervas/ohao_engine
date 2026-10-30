"""Tree units: 07 · Sampling."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'sampling',
    "title": '07 · Sampling',
    "hub": 'sampling.html',
    "children": [
        page(
            'sobol',
            'Sobol + Owen',
            'Host-side Sobol generator and Owen scramble, tested by sobol_test.',
            files=['ohao/render/rt/sobol_generator.cpp', 'ohao/render/rt/sobol_generator.hpp', 'ohao/render/rt/owen_scramble.cpp', 'ohao/render/rt/owen_scramble.hpp'],
            design=['Sobol sequences + Owen scrambling on the CPU; no shader reads them: the path tracer draws from its own PCG stream.', 'sobol_test checks the generator and the scramble.'],
        ),
        page(
            'pcg',
            'PCG stream',
            'The path tracer\'s hash-based random stream.',
            files=['shaders/rt/path_tracer.slang'],
            design=['path_tracer.slang carries its own PCG stream (Rng), seeded per pixel and sample; 24-bit floats in [0, 1).', 'The any-hit shader hashes the ray and triangle with the same function for stochastic opacity.'],
        ),
        page(
            'mis',
            'MIS heuristics',
            'The power heuristic in path_tracer.slang.',
            files=['shaders/rt/path_tracer.slang'],
            design=['Power heuristic for light and environment sampling against BSDF sampling, on both sides (NEE and the bounce that meets an emitter or escapes).', 'Weights use pdf_light and pdf_bsdf in solid angle at the same direction — unit mismatch = bias.'],
        ),
        page(
            'env-cdf',
            'Environment CDF',
            'CPU build + the path tracer\'s sampleEnv/envPdf.',
            files=['ohao/render/rt/env_cdf.cpp', 'ohao/render/rt/env_cdf.hpp', 'shaders/rt/path_tracer.slang'],
            design=['CPU builds marginal + conditional CDFs from HDR luminance.', 'path_tracer.slang\'s sampleEnv / envPdf: a texel by the CDF, a point uniform in it; the density weighs NEE against escaped bounce rays.'],
        ),
    ],
}
