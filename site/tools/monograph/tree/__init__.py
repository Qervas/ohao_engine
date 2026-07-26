"""Per-module tree units; assembled into TREE by tree_data."""
from __future__ import annotations

from . import (
    architecture,
    core,
    scene,
    gpu,
    materials,
    deferred,
    path_tracer,
    sampling,
    hybrid,
    denoise,
    shaders,
    camera,
    graph,
    physics,
    audio,
    systems,
)

MODULES = [
    architecture.MODULE,
    core.MODULE,
    scene.MODULE,
    gpu.MODULE,
    materials.MODULE,
    deferred.MODULE,
    path_tracer.MODULE,
    sampling.MODULE,
    hybrid.MODULE,
    denoise.MODULE,
    shaders.MODULE,
    camera.MODULE,
    graph.MODULE,
    physics.MODULE,
    audio.MODULE,
    systems.MODULE,
]
