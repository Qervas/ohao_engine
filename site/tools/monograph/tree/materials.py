"""Tree units: 04 · Materials."""
from __future__ import annotations

from ..schema import page

MODULE = {
    "id": 'materials',
    "title": '04 · Materials',
    "hub": 'materials.html',
    "children": [
        page(
            'pbr-model',
            'PBR metallic-roughness',
            'F0, ORM convention, roughness floors.',
            files=['shaders/material/openpbr.slang', 'ohao/render/scene/material_table.hpp'],
            design=['OpenPBR\'s base (material/openpbr.slang) in the path tracer and deferred lighting: base_color/weight, metalness, specular_color/weight/roughness/ior, diffuse roughness.', 'Dielectric reflectance from specular_ior (1.5 gives F0 0.04); metals use F82-tint with base colour as F0.', 'ORM packing: occlusion/roughness/metal in one texture (glTF convention).', 'OpenPBR floors specular roughness at 0.01 (kMinSpecularRoughness); the G-buffer stores it as authored.'],
        ),
        page(
            'ggx',
            'GGX implementation',
            'D/F/G terms, Smith correlated, VNDF sampling, energy compensation.',
            files=['shaders/material/openpbr.slang'],
            design=['openpbr.slang: Trowbridge-Reitz D, height-correlated Smith G2, F82-tint and exact dielectric Fresnel, bounded VNDF sampling, Kulla-Conty multiple scattering from tabulated energies.', 'One GGX for the path tracer, deferred lighting, the transparent pass and RT GI hits — one visual language.'],
        ),
        page(
            'pack',
            'GPU material pack',
            '3×vec4 matColors, texture bit-cast indices.',
            files=['ohao/gpu/layout_meta.hpp', 'ohao/render/rt/gpu_light.hpp'],
            design=['Material rows are tightly packed vec4s; texture indices bit-cast into floats for SSBO compatibility.', "Bindless index 0xFFFFFFFF means 'no texture' — shaders must branch before sample.", 'layout_meta asserts sizes match GLSL std430 expectations.'],
        ),
        page(
            'lights',
            'Lights dual packing',
            'GPULight SSBO (64 lights) vs the deferred LightGpu table (65,536).',
            files=['ohao/render/rt/gpu_light.hpp', 'ohao/render/scene/light_table.cpp', 'shaders/shared/layout.json'],
            design=['Path tracer: GPULight, 80 bytes, std430 SSBO behind a 16-byte header (position, type, color, dir, spot/area extras).', 'Deferred: LightGpu, 64 bytes, a storage buffer per frame slot read through the froxel lists.', 'LightTable::set packs the scene\'s LightDescs into both layouts.', 'Sphere lights use radius in dirAndParam.w; NEE must use distance for shadow Tmax, not radius.'],
        ),
    ],
}
