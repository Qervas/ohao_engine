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
            files=['shaders/includes/brdf/brdf_ggx.glsl', 'shaders/includes/pbr_unpack.glsl'],
            design=['Metallic-roughness: baseColor, metallic, roughness → diffuse + specular F0.', 'Dielectric F0 ≈ 0.04; metals use baseColor as F0 with zero diffuse.', 'ORM packing: occlusion/roughness/metal in one texture (glTF convention).', 'Roughness floor prevents zero-width specular fireflies in PT.'],
        ),
        page(
            'ggx',
            'GGX implementation',
            'D/F/G terms, Smith correlated, energy helpers.',
            files=['shaders/includes/brdf/brdf_ggx.glsl', 'shaders/includes/brdf/brdf_common.glsl', 'shaders/includes/material/ggx_aniso.glsl'],
            design=['Trowbridge-Reitz D, Schlick F, Smith G (correlated form in brdf_ggx.glsl).', 'Same GGX used by deferred lighting and path-tracer lobes — one visual language.', 'ggx_aniso.glsl extends to anisotropic roughness for brushed metals.'],
        ),
        page(
            'pack',
            'GPU material pack',
            '3×vec4 matColors, texture bit-cast indices.',
            files=['ohao/gpu/layout_meta.hpp', 'ohao/render/rt/gpu_light.hpp'],
            design=['Material rows are tightly packed vec4s; texture indices bit-cast into floats for SSBO compatibility.', "Bindless index 0xFFFFFFFF means 'no texture' — shaders must branch before sample.", 'layout_meta asserts sizes match GLSL std430 expectations.'],
        ),
        page(
            'advanced',
            'Advanced material includes',
            'advanced_brdf, material_sampling, material_types.',
            files=['shaders/includes/material/advanced_brdf.glsl', 'shaders/includes/material/material_sampling.glsl', 'shaders/includes/material/material_types.glsl'],
            design=['material_sampling: importance sample GGX VNDF / cosine hemisphere for PT.', 'material_types: enum-like constants shared across hit and raygen.', 'advanced_brdf: multi-lobe helpers beyond simple metal-rough.'],
        ),
        page(
            'lights',
            'Lights dual packing',
            'GPULight SSBO (64) vs deferred LightData UBO (8).',
            files=['ohao/render/rt/gpu_light.hpp', 'ohao/gpu/vulkan/light_upload.cpp', 'shaders/includes/lighting/light_types.glsl'],
            design=['Path tracer: GPULight 64-byte SSBO (position, type, color, dir, spot/area extras).', 'Deferred: compact LightData UBO for the forward-compatible light loop.', 'light_upload.cpp packs scene LightComponents into both layouts as needed.', 'Sphere lights use radius in dirAndParam.w; NEE must use distance for shadow Tmax, not radius.'],
        ),
    ],
}
