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
            'Owen-scrambled Sobol',
            "Burley's shuffled, Owen-scrambled 4-D Sobol in sobol.slang: the path tracer's sampler, drawn in groups of four.",
            files=['shaders/sampling/sobol.slang', 'shaders/rt/path_tracer.slang', 'ohao/render/rt/sobol_generator.cpp', 'ohao/render/rt/owen_scramble.cpp', 'tools/sampling/sobol_kernel.slang', 'tests/renderer/sobol_test.cpp'],
            design=["Joe-Kuo direction numbers for dimensions 0-3; Burley 2020's nested uniform scramble (Vegdahl's constants) on the shuffled index and each coordinate.", 'Sampler draws in groups of four, one 4-D point a group seeded by hashCombine(pixelSeed, group): group 0 the jitter; bounce d: 1+3d light, 2+3d environment, 3+3d BSDF and Russian roulette.', 'CPU twin: sobol_generator.* and owen_scramble.*, and the module compiled to C++ by slangc (tools/sampling, ohao_sobol_cpu); sobol_test holds the shader bit for bit to them.'],
        ),
        page(
            'pcg',
            'PCG hash',
            "The PCG permutation as a hash: the pixel seed, the any-hit coin, the hybrid traces' keyed draws.",
            files=['shaders/rt/path_tracer.slang', 'shaders/rt/shadow_trace.slang', 'shaders/rt/gi_trace.slang', 'shaders/rt/shadow_filter.slang'],
            design=["path_tracer.slang: the pixel seed of the Sobol sequence and the any-hit shader's stochastic-opacity coin.", 'shadow_trace, gi_trace and shadow_filter carry their own copies for stateless keyed draws.'],
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
