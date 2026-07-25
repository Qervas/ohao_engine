"""Tree units: Camera."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'camera',
    "title": 'Camera',
    "hub": None,
    "children": [
        page(
            'camera',
            'Camera',
            'View/proj, FPS/orbit style controls used by examples.',
            files=['ohao/render/camera/camera.hpp', 'ohao/render/camera/camera.cpp'],
        ),
        page(
            'scene-framer',
            'Scene framer',
            'Auto-frame bounds for turntable/model viewer.',
            files=['ohao/render/camera/scene_framer.hpp', 'ohao/render/camera/scene_framer.cpp'],
        ),
    ],
}
