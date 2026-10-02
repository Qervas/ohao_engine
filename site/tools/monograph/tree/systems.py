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
            design=['Top-level options gate NRD, OIDN, DLSS, Jolt, tests.', 'shaders custom target compiles GLSL → SPIR-V into bin/shaders.', 'examples link ohao libraries; ohao_game is the windowed host.'],
        ),
        page(
            'examples',
            'Examples map',
            'cornell_box, model_viewer, env_demo, turntable, ohao_game.',
            files=['examples/cornell_box.cpp', 'examples/model_viewer.cpp', 'examples/env_demo.cpp', 'examples/turntable.cpp', 'examples/ohao_game.cpp', 'examples/example_cli.hpp'],
            design=['example_cli.hpp shared flags: model, env, mode, denoise, spp.', 'ohao_game: windowed deferred host (WASD/QE fly camera, --frames/--resize-at presentation gate).', 'cornell_box / turntable feed golden and showcase pipelines.', 'Research-only CLIs under examples/ are out of the public monograph scope.'],
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
            files=['STATUS.md@223ff7f', 'docs/bugs_solved'],
            design=['STATUS.md was the source of truth for what is proven vs experimental; removed at 2bd0bac, cited here at 223ff7f.', 'docs/bugs_solved archives root-cause writeups (NRD pack, OOM, etc.).'],
        ),
    ],
}
