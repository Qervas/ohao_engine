# Khronos validation scene pack

**Spec:** none; Frank's standing instruction of 2026-10-25 (12:08 Berlin): add
glTF-Sample-Models MetalRoughSpheres, FlightHelmet and Sponza only, from the staged
checkout in `ohao_refs/khronos_staging` (its `LICENSE_NOTES.md` and `MATERIAL_SUPPORT.md`),
each as a scene and an example path with live and reference goldens, licenses
documented. Also Frank's ruling of 2026-10-25 on `window_capture_rt_inline_ms`.
**Status:** Built and tested. Adding the pack exposed one loader/shader bug, which is
fixed: ambient occlusion was read from the wrong texture. The fix re-baselines the
helmet goldens. It also uncovered a second gap, which stays open: the deferred frame's
specular IBL is neither occluded nor prefiltered by roughness (section 6). The five clean
bench runs are still open; the machine was loaded by another job.

## 1. Summary

- **Assets:**
  - `assets/khronos/{MetalRoughSpheres,FlightHelmet,Sponza}/` holds each model's
    `glTF/` files from glTF-Sample-Models commit `d7a3cc8`, unmodified: 11,229,274 +
    48,395,655 + 52,697,969 bytes, the same counts as the staging notes.
  - Each directory also holds the upstream README as `README.upstream.md` and a
    `LICENSE.md`; the pack has a `README.md` with the credit table.
  - `.gitattributes` pins `assets/khronos/**` to `-text`, so the files stay byte for byte.
- **Scenes:**
  - `assets/scenes/khronos_metal_rough_spheres.scene.json` and
    `khronos_flight_helmet.scene.json` are `model_viewer --save-scene` output: the model
    framed in the viewer's room, so the example path and the scene file render the same
    pixels.
  - `khronos_sponza.scene.json` is written by hand. The framer would shrink Sponza into
    its room and look at an outside wall. Instead the model sits at its own scale (about
    30 m by 18 m), with a camera inside the atrium at (4, 1.8, 0), looking down the nave
    (−X) at fov 70.
  - Sponza's lighting: a directional sun through the open roof (intensity 3), five
    atrium point lights at y = 2.5 (intensity 40, range 10), and the outdoor HDRI.
- **Goldens:** 8 new manifest entries and 6 new images, all exact (max_abs 0).
  - `khronos_metal_rough_spheres_deferred` and `khronos_flight_helmet_deferred` run the
    model_viewer example; their `cli_` twins render the scene files to the same images.
  - `cli_khronos_sponza_deferred` covers Sponza's live frame.
  - The three `..._reference` entries render at reference quality.
  - Every entry uses 30 deferred frames with TAA, SSAO and bloom at exposure 1.
  - The corpus now holds 22 scenes.
- **Bug fixed: occlusion.**
  - Before: the G-buffer took ambient occlusion from the metallicRoughness map's R
    channel. glTF leaves that channel unused and keeps occlusion in a separate
    `occlusionTexture`.
  - Now: the loader reads `occlusionTexture` (Assimp's `LIGHTMAP`). If it is the same
    image as the metallicRoughness map (packed ORM), it is uploaded once and both slots
    use it.
  - The occlusion index is packed into the material table's last free lane (row 2, w),
    and `gbuffer.frag` reads AO only from it.
- **Budget:** `window_capture_rt_inline_ms` is gated at 150 ms (Frank's ruling) in
  `tests/bench/serve_budgets.json`. `serve_bench` checks it with the other window
  measures instead of only recording it. No other budget changed.
- **Docs:**
  - `AGENTS.md`: the golden count, the new serve_bench measure, and a gotcha on
    occlusion and IBL.
  - `ohao-cli.md`: a "Validation scenes" section whose commands `check_docs` runs.
  - Monograph: five material units re-cited (`deferred/post`, `gpu/scene-upload`,
    `gpu/layout-meta`, `materials/pack`, `deferred/ssr-sss`).

## 2. What was built

| File | Change |
|---|---|
| `assets/khronos/**` | the three models, `README.md`, a `LICENSE.md` and `README.upstream.md` each |
| `assets/scenes/khronos_*.scene.json` | the three scenes |
| `.gitattributes` | `assets/khronos/** -text` |
| `ohao/scene/asset/model.hpp`, `model_loader.cpp` | `occlusionTextures`, `materialOcclusionTexIndex`, `kOcclusionIsRoughMetal`; the glTF occlusion load; ufbx-textured FBX drop Assimp's occlusion |
| `ohao/render/scene/render_scene_types.hpp`, `material_table.cpp`, `model_upload.cpp`, `ohao/gpu/layout_meta.hpp` | `MaterialDesc::occlusionTex`, packed in row 2, w; uploaded or shared with the roughness-metal texture |
| `shaders/core/gbuffer.frag`, `shaders/rt/pt_closesthit.rchit` | AO from the occlusion texture only; the path tracer's unused AO line removed |
| `tests/engine/engine_tests.cpp` | the material pack test checks the occlusion lane |
| `tests/golden/manifest.json` + 6 PNGs | the pack's goldens; notes on the three re-baselined helmet goldens |
| `tools/bench/serve_bench.cpp`, `tests/bench/serve_budgets.json` | `window_capture_rt_inline` gated at 150 ms |

## 3. Bugs found and fixed

- **AO from the metallicRoughness R channel.** MetalRoughSpheres' map has R = 0.1/255 on
  average, so every sphere and label panel rendered black in the deferred frame.
  DamagedHelmet's map has R = 0.9/255 on average, while its real occlusion map averages
  225/255. So the helmet in `damaged_helmet`, `helmet_game` and `ohao_game` has never
  received ambient or IBL light in the deferred frame.
  - Fix: AO comes from glTF's occlusion texture only (section 1).
  - Goldens re-baselined in their own commit: `damaged_helmet_deferred` and its CLI twin,
    `cli_helmet_game_deferred` and `cli_helmet_game_reference`, max_abs 251 on
    37.1–37.6% of pixels, rmse 66.8. I looked at them before the update.
  - The helmet now reads as white chrome: see section 6.

## 4. Spec corrections

None: no spec covers the pack. Two of the staging notes' cautions were applied rather
than resolved:
- **Sponza's license grant.** The staging notes advised keeping Sponza in staging until
  its non-standard grant was accepted. It was copied at Frank's explicit instruction.
  `Sponza/LICENSE.md` carries the full credit chain and the verbatim notes, and flags it
  for review before any public release.
- **Alpha.** The lenses and leaves render opaque, as `MATERIAL_SUPPORT.md` predicted.

## 5. Results

- **Golden corpus:** 22 scenes, every one passing.
  - The 8 new scenes are max_abs 0.
  - Example and CLI twins are identical (MetalRoughSpheres measured before capture:
    max_abs 0 over 1920x1080).
  - The 14 earlier scenes are unchanged except the four helmet scenes re-baselined in
    section 3.
- **`tools/check.py`:** 37/37, including engine_tests 84/84 (section 7 has the final run).
- **Example and CLI renders** (1920x1080, deferred, 10 frames, GPU time per frame):
  MetalRoughSpheres 1.97 ms, FlightHelmet 1.82 ms, Sponza 3.78 ms.
- **`serve_bench --check`** (noisy: calibration more than 25% over its reference):
  - `window_capture_rt_inline` 124.0 ms median, p95 134.5 (budget 150);
  - `window_readback` 1.32 ms (budget 2);
  - `window_capture_inline` 9.27 ms (budget 20);
  - `window_capture_png` 8.83 ms (budget 30);
  - `window_step_present` 0.35 ms (budget 8).
- The five consecutive clean runs were not attempted: another job is loading the
  machine.

## 6. Known gaps (what the pack shows)

- **Specular IBL.**
  - It is unoccluded: inside a closed room the metal mirrors the studio HDRI instead of
    the room.
  - It is unprefiltered: the environment has one mip level, so `lod = roughness * 9`
    samples the same level at every roughness, and every metal is a sharp mirror.
  - Evidence: DamagedHelmet now reads as white chrome where the path-traced golden shows
    dark teal metal. MetalRoughSpheres' deferred spheres do not change across the
    roughness axis, while the path tracer shows the gradient.
  - The previous frame hid this on the helmet only because AO was 0.
  - Next: a prefiltered environment plus specular occlusion, or RT reflections.
- **SSR on curved metal.** SSR draws cross-shaped streaks on MetalRoughSpheres' gold
  spheres; they disappear with SSR off.
- **Alpha.** FlightHelmet's `LensesMat` (BLEND) and Sponza's three MASK materials
  render opaque: there is no alpha test or blend path.
- **Not validated:** `KHR_materials_*` (clearcoat, transmission, IOR, volume,
  specular, sheen, iridescence, anisotropy). ClearCoatTest and TransmissionTest are
  out for that reason.
- **The material-pack figure.** The monograph's `materials-pack-matrow` figure still
  draws row 2, w as unused.

## 7. How to run the new tests

```
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_khronos_sponza_deferred
python tests/golden/render_golden.py tests/golden/manifest.json      # all 22
build/Release/ohao validate assets/scenes/khronos_flight_helmet.scene.json --check-assets
build/Release/model_viewer.exe assets/khronos/FlightHelmet/FlightHelmet.gltf out.png 4 deferred --denoise=none
build/Release/serve_bench.exe --check tests/bench/serve_budgets.json   # window_capture_rt_inline <= 150 ms
```

## 8. Commits

See `git log`: the occlusion fix, the helmet golden re-baseline, the pack, the
`window_capture_rt_inline` budget, and this record with the docs.
