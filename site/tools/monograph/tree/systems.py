"""Tree units: Build & status."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'systems',
    "title": 'Build & status',
    "hub": None,
    "children": [
        page(
            'build',
            'Build system',
            'CMake options, targets, shaders target, examples.',
            files=['CMakeLists.txt', 'ohao/CMakeLists.txt', 'examples/CMakeLists.txt', 'shaders/CMakeLists.txt'],
            design=['Top-level options gate NRD, OIDN, DLSS, Jolt, tests.', 'shaders custom target compiles GLSL → SPIR-V into bin/shaders.', 'examples link ohao libraries; interactive is the full-featured driver.'],
        ),
        page(
            'examples',
            'Examples map',
            'cornell_box, model_viewer, env_demo, interactive, turntable.',
            files=['examples/cornell_box.cpp', 'examples/model_viewer.cpp', 'examples/env_demo.cpp', 'examples/interactive.cpp', 'examples/turntable.cpp', 'examples/example_cli.hpp'],
            design=['example_cli.hpp shared flags: model, env, mode, denoise, spp.', 'interactive: WASD camera + mode switch + --denoise=dlssrr.', 'cornell_box / turntable feed golden and showcase pipelines.', 'Research-only CLIs under examples/ are out of the public monograph scope.'],
        ),
        page(
            'tests',
            'Tests & goldens',
            'Unit suites and golden image gate.',
            files=['tests/CMakeLists.txt', 'tests/golden', 'tests/engine', 'tests/renderer'],
            design=['Golden images gate path-tracer and deferred regressions.', 'Engine/renderer unit tests cover upload, materials, AS invariants.'],
        ),
        page(
            'status',
            'Status discipline',
            'Evidence-based feature matrix protocol.',
            files=['STATUS.md', 'docs/bugs_solved'],
            design=['STATUS.md is the source of truth for what is proven vs experimental.', 'docs/bugs_solved archives root-cause writeups (NRD pack, OOM, etc.).'],
        ),
        page(
            'public-scope',
            'Public scope note',
            'What the monograph deliberately omits.',
            topics=['Private research trees under ohao/ (not listed as product chapters)', 'ohao/render/diff/** — experimental differentiable helpers', 'Research-only example binaries', 'shaders/_disabled/** — retired experiments'],
            design=['Everything else under ohao/ and active shaders/ is mapped in this tree.', 'Keep non-product research documentation offline; this site is the public engine face.'],
        ),
    ],
}
