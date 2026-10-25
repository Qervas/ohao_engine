# Khronos validation pack

Three glTF 2.0 sample models from the Khronos Group's
[glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) repository, at
commit `d7a3cc8e51d7c573771ae77a57f16b0662a905c6`. They are standard scenes for checking
that OHAO loads and shades core metallic-roughness glTF the way other renderers do.
Khronos archived that repository on 2023-12-22; its successor is
[glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets).

| Directory | Model | License | Credit |
|---|---|---|---|
| `MetalRoughSpheres/` | Metal-Rough Spheres | CC BY 4.0 | Copyright 2017 Analytical Graphics, Inc. Model and textures by Ed Mackey. |
| `FlightHelmet/` | Flight Helmet | CC0 1.0 | Donated by Microsoft for glTF testing. |
| `Sponza/` | Sponza | Crytek's public donation (see `Sponza/LICENSE.md`) | Marko Dabrovic (original), Frank Meinl / Crytek (2010 atrium), Morgan McGuire (2011 cleanup), Alexandre Pestana (PBR textures). |

Each directory holds that model's `glTF/` files exactly as published (no file is
modified), its upstream `README.md` renamed `README.upstream.md` (the license statement
Khronos points to), and a `LICENSE.md` with the credit above. `.gitattributes` keeps every
file here byte for byte. The pack is not endorsed by Khronos, Analytical Graphics,
Microsoft or Crytek.

## What uses them

| Scene | Built how | Goldens (`tests/golden/manifest.json`) |
|---|---|---|
| `assets/scenes/khronos_metal_rough_spheres.scene.json` | `model_viewer --save-scene` (framed in the model viewer's room) | `khronos_metal_rough_spheres_deferred` (example and CLI), `..._reference` |
| `assets/scenes/khronos_flight_helmet.scene.json` | `model_viewer --save-scene` | `khronos_flight_helmet_deferred` (example and CLI), `..._reference` |
| `assets/scenes/khronos_sponza.scene.json` | by hand: the model at its own scale (about 30 m), a camera in the atrium, a sun and five atrium lights | `khronos_sponza_deferred`, `..._reference` |

## What they exercise, and what they do not

Core metallic-roughness: base color, metallicRoughness (G roughness, B metallic),
normal, emissive and occlusion textures, node transforms, double-sidedness. Not loaded or
shaded, so not validated by this pack: `KHR_materials_*` extensions (clearcoat,
transmission, volume, IOR, specular, sheen, iridescence, anisotropy) and alpha.
FlightHelmet's `LensesMat` is `alphaMode: BLEND` and three Sponza materials are
`alphaMode: MASK`; the frame has no alpha path, so those surfaces render opaque.
ClearCoatTest and TransmissionTest were deliberately left out for that reason.
