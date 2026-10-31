# OpenPBR step 2: coat, fuzz and anisotropy

**Spec:** `docs/specs/2026-10-29-openpbr-materials-design.md` (sections 2–8, step 2; rulings O1–O5). Step 1:
`docs/changes/2026-10-29-openpbr-step1.md`. The frontier renderer (F0–F6) is done, so every consumer of the module is
Slang: the path tracer (both profiles), deferred lighting, the clustered loop the forward pass shares, RT direct light's
shading, RT GI's hits and its reflection.
**Status:** T1–T4 and T6 built (2026-10-31): `docs/changes/2026-10-31-openpbr-step2.md`; T5 (LTC area lights) moves to its own step. Frank: "openpbr yes, go on". Rulings O6–O9 (below) accepted by Frank on 2026-10-31 ("all four").

**Goal:** OpenPBR Surface 1.1.1's coat, fuzz and specular anisotropy in the module, the path tracer as a correct
estimator of them, and the deferred frame shading them, held by the same closed forms as step 1. `KHR_materials_clearcoat`,
`_sheen` and `_anisotropy` import onto them. A car paint, a velvet and a brushed metal enter the goldens.

Every task ends with the engine running and every gate green. A task that moves goldens says which and why, and looks at
them before `--update`.

## The model (OpenPBR 1.1.1, as the spec writes it)

From the top: `M = layer(layer(base, coat, C), fuzz, F)`.

- **Fuzz** (`fuzz_weight` F 0, `fuzz_color` 1, `fuzz_roughness` 0.5): `F f_fuzz + (1 − F E_fuzz(μo, r)) f_coated`, with
  `μi f_fuzz = fuzz_color E_fuzz(μo, r) D_ltc(ωi | μo, r)` (Zeltner, Burley & Chiang 2022). The attenuation uses the
  untinted albedo: the base is never coloured by `fuzz_color`. `E_fuzz` and the LTC's inverse matrix are MaterialX's fits
  (`mx_microfacet_sheen.glsl`, from `tizian/ltc-sheen`; Apache-2.0, ported with its notice as EON's listings were).
- **Coat** (`coat_weight` C 0, `coat_color` 1, `coat_roughness` 0, `coat_roughness_anisotropy` 0, `coat_ior` 1.6,
  `coat_darkening` 1): a GGX dielectric (the base's dielectric code at η = `coat_ior`, white). Below it
  `lerp(1, T (1 − E_coat(μo)), C)` times the coated base, with
  - absorption `T = coat_color^((1/μti + 1/μto) / 2)`, `μt = sqrt(1 − (1 − μ²) / ηc²)` (`coat_color` the transmittance
    at normal incidence there and back);
  - darkening `lerp(1, Δ, C δ)`, `Δ = (1 − K) / (1 − E_b K)`, `K = lerp(F(μo, ηc), 1 − (1 − Favg(ηc)) / ηc², r_b)`,
    `r_b = lerp(lerp(1, r, ξs Fs), r, M)`, `E_b` the base's albedo at normal incidence;
  - the base's specular IOR ratio `lerp(n_b, n_b / n_c, C)` (inverted past TIR: `n_c / n_b` when `n_c > n_b`);
  - the base's roughness `lerp(r_B, min(1, r_B⁴ + 2 r_C⁴)^¼, C)`.
- **Anisotropy** (`specular_roughness_anisotropy` a 0, and the coat's): `α_t = r² sqrt(2 / (1 + (1 − a)²))`,
  `α_b = (1 − a) α_t`, the NDF stretched along the tangent. GGX's D, Λ and bounded VNDF sampling (Eto & Tokuyoshi 2023)
  take (α_t, α_b). The energy tables stay isotropic, read at `r` (OpenPBR's mapping keeps α_t² + α_b² = 2α²); the furnace
  says how far that holds.
- **The tangent:** OpenPBR's `geometry_tangent` is the mesh's tangent rotated about the normal by
  `geometry_tangent_rotation` (turns, glTF's `anisotropyRotation` / 2π); the coat shares it.

Lobe selection for sampling is by luminance of each lobe's albedo (fuzz, coat, base specular, diffuse).

## Tasks

### T1. The module and its CPU tests

- `shaders/material/openpbr.slang`:
  - `MaterialInputs` grows by the coat, fuzz and anisotropy fields; `defaultMaterialInputs()` is OpenPBR 1.1.1's.
  - GGX takes two alphas (`ggxD`, `ggxLambda`, `ggxSingle`, `ggxSampleBounded`, `ggxPdfBounded`); the isotropic case is
    the same arithmetic (step 1's goldens move by at most rounding; the path tracer's are re-captured after looking).
  - `OpenPBRBase` keeps its job (the base) with α_t, α_b; `struct OpenPBRSurface` is the whole: fuzz over coat over base,
    with `evalLobes(wo, wi) → specular, diffuse` (coat and base specular in the first, fuzz and the diffuse in the second:
    RT direct light's two demodulated channels), `eval`, `sample`, `pdf`, `albedo`, `specularAlbedo`, `diffuseAlbedo`,
    and `withLightSize(...)` widening each specular lobe by its own roughness (a sphere's or a rectangle's representative
    point). Every consumer moves from `OpenPBRBase` to it.
- `tests/material/material_tests.cpp` (the module's C++ build):
  - χ² of `sample()` against `pdf()` for coat, fuzz, anisotropic base and all together;
  - `albedo()` against the integral of `eval()` within 0.5%;
  - the furnace: a white coat (`coat_color` 1; `coat_darkening` 0 and 1: over a white base Δ is 1) over a white dielectric and a
    white metal, roughness 0.01 to 1, views to 85°, within 1%; anisotropy 0, 0.5, 0.9 within 1% (if the isotropic tables miss
    it, the error is recorded and the tables gain an anisotropy axis);
  - fuzz over a white base within 1% (white fuzz's albedo scaling is energy-exact by construction);
  - the defaults equal to OpenPBR 1.1.1's.
- `openpbr_tables --check` unchanged unless a table is added.

### T2. Data: `Material`, the material table, import

- `ecs::Material` adds `coat_weight`, `coat_color`, `coat_roughness`, `coat_roughness_anisotropy`, `coat_ior`,
  `coat_darkening`, `fuzz_weight`, `fuzz_color`, `fuzz_roughness`, `specular_roughness_anisotropy`,
  `geometry_tangent_rotation` (OpenPBR's defaults). Files that omit them keep their look; `schemaHash` moves (CLI and
  serve expectations re-captured; nothing else in them may change).
- `render::MaterialDesc` and the table: `kMaterialRows` 7 → 10:
  - row 7: `coat_color`, `coat_weight`;
  - row 8: `coat_roughness`, `coat_roughness_anisotropy`, `coat_ior`, `coat_darkening`;
  - row 9: `fuzz_color`, `fuzz_weight`; the remaining scalars (`fuzz_roughness`, the anisotropy and its rotation) and the
    new textures' indices packed where step 1 packed its own (the plan's T2 commit names each slot in `layout.json`).
- Import (`ohao/scene/asset/model_loader.cpp`): Assimp 5.4.3 reads `KHR_materials_clearcoat` (factor, roughness, the
  two textures) and `_sheen` (colour, roughness, textures): clearcoat → coat (`coat_ior` 1.5 and `coat_darkening` 0: glTF's
  clearcoat has neither), sheen → fuzz (`fuzz_weight` 1 where the colour is not black). Assimp does not read
  `KHR_materials_anisotropy`: a small reader of that one extension (simdjson, beside `level/gltf_structure`) takes
  strength and rotation; its texture is a known gap until the material reader of the spec's section 4 exists.
  `W_LEVEL_UNSUPPORTED` stops listing the three.
- Instantiate's material cache keys on every new value.

### T3. The path tracer and RT GI's hits

- `rt/path_tracer.slang` builds `OpenPBRSurface` at every vertex (the hit's tangent frame for anisotropy: the vertex
  tangents normal maps already use, rotated by `geometry_tangent_rotation`); NEE, BSDF sampling and MIS unchanged in form.
- `rt/alpha_test.slang`'s `hitMaterial` resolves the new rows (and textures); RT GI's hit bounces
  `OpenPBRSurface.albedo` at normal incidence.
- `check_material.py`: the white furnace in the path tracer adds a coated dielectric, a coated metal, white fuzz over a
  dielectric, an anisotropic metal (within 1%).

### T4. The deferred frame

- **The G-buffer's eighth colour target** (`kLayers`, RG32_UINT; the GPU allows 8 colour attachments, the G-buffer uses
  7): word 0 the material's index (16 bits), the anisotropy (8 bits) and the tangent's angle about the normal in a
  basis made from the normal alone (8 bits, over [0, π): the NDF is symmetric under t → −t); word 1 the coat's weight
  and roughness and the fuzz's weight and roughness (8 bits each, textures applied). The colours, `coat_ior`,
  `coat_darkening` and the coat's anisotropy come from the material table by index (ruling O6).
- `includes/lighting/gbuffer_material.slang` decodes it; `deferred_lighting.slang`, `clustered_lights.slang` (and so the
  transparent pass) and `restir_di.slang` shade `OpenPBRSurface`. Analytic lights evaluate it exactly; spheres and
  rectangles by representative point per specular lobe.
- **Indirect light:** RT GI's diffuse times `diffuseAlbedo` (fuzz included); RT GI's one reflection ray follows the coat's
  lobe where `coat_weight > 0` and the base's elsewhere, and the base's specular under a coat takes the same radiance
  (ruling O8); the split sum (no ray queries) weighs each specular lobe's prefiltered level by its own roughness.
- `check_material.py`: the same furnace cases in deferred within 1%. `check_analytic.py`: a coated floor and a fuzz floor
  under one light against `tests/renderer/openpbr_ref.py` (grown by the new lobes) within 3% in both renderers.

### T5. Area lights by linearly transformed cosines

- Heitz, Dupuy, Hill & Neubelt 2016: a rectangle's specular by the LTC fitted to the module's own GGX (isotropic, single
  scatter, with Fresnel's split), the diffuse by the exact polygon irradiance as today. The fit is our own tool
  (`tools/material/ltc_fit`, the module's C++ build, Nelder–Mead as the paper's), its 64×64 table generated, checked in
  and held byte for byte as `openpbr_tables` is. Sphere lights keep the representative point (no sphere LTC).
- `check_analytic.py`'s `area_light` cases tighten to the path tracer at grazing views; the referee's `area_light` row.

### T6. Scenes, goldens, the referee, docs

- `assets/scenes/materials_step2.scene.json`: a car paint (blue metallic base, `specular_roughness` 0.4, coat 1 at
  roughness 0.03), velvet (fuzz 1, red, roughness 0.5, over a dark base), a brushed metal (anisotropy 0.8) and a
  lacquered wood, lit by the sky and an area light; deferred, reference and path traced in the goldens.
- The referee re-run (table in the change record); `render_bench` before and after (`gbuffer`, `lighting`, `di.shade`,
  `rt.gi`), budgets proposed, not set.
- AGENTS.md (the G-buffer's targets, the module, `Material`'s fields), ARCHITECTURE.md, ROADMAP row 17, the change
  record `docs/changes/2026-10-31-openpbr-step2.md`.

## Rulings (accepted, Frank, 2026-10-31: "all four")

- **O6, the G-buffer.** One RG32_UINT target carries the material's index and the per-pixel layer values; colours and
  IORs come from the table, so the coat's and the fuzz's colour textures are not seen by deferred (the path tracer sees
  them). The visibility buffer (O3) would remove the limit; proposed after OpenPBR step 3, when transmission adds its own
  inputs.
- **O7, the fuzz's fits.** MaterialX's analytic fits of Zeltner's tables (Apache-2.0, with its notice), not a table of our
  own fit to a volumetric simulation.
- **O8, one reflection ray.** Under a coat RT GI's reflection serves the coat (the sharp lobe the eye reads); the base's
  specular below takes RT GI's mean incoming radiance, blended toward the coat's ray as the two roughnesses meet (reusing
  the coat's sharp ray made a rough metal under a smooth coat a mirror). A second ray is 0.5–1 ms at 1080p on Sponza; the referee measures what one costs in
  error.
- **O9, `geometry_tangent_rotation`.** The one non-OpenPBR field: a rotation of the mesh's tangent in turns, as glTF's
  anisotropy has, since a scene file cannot author a tangent field.

## Tests

- `material_tests` (T1), `check_material.py` (T3, T4), `check_analytic.py` (T4, T5), the goldens (T6, looked at),
  `check_parity.py`'s table, validation clean, `tools/check.py` green after every task.
