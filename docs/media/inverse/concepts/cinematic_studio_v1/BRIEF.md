# Scene brief — cinematic museum studio (v1)

**Status:** Phase 1 **implemented** — `--preset museum` + NIUA assets under `assets/museum_studio/`.  
Smoke: meshes load (statue+pedestal); dense MAPTEST PSNR gain OK, map MSE still tight (heavy mesh / optim).  
**Domain label:** `ohao_museum_studio_protocol` (not a public IR benchmark).

## Direction (updated after NIUA pack)

| Track | Assets | Look |
|-------|--------|------|
| **Primary (new)** | NIUA museum pack from `untitled_1` | Dark cinematic **museum gallery** — marble tile floor, stone pedestal, classical prop hero |
| Secondary (earlier) | Helmet / boombox / spheres | Still valid as quality-plate rows; not the new hero set |

### Why museum pack beats lantern/spheres for “ideal scene”

1. **Floor is already a real material language** — black/white marble tiles (`floor_albedo.jpg`) = strong multi-view albedo signal and recoverable free-grid pattern  
2. **Hero props are product-photography native** — statue / amphora / bust on pedestal  
3. **Pedestal glTF** replaces the procedural box  
4. **Optional dress** — column / arch as fixed non-θ set pieces  
5. Still fits engine contract: free θ = **ground dense maps**; hero fixed beauty

## Asset source

| Role | Source path | Import target (Phase 1) |
|------|-------------|-------------------------|
| Hero A | `…/untitled_1/assets/props/statue.glb` | `assets/museum_studio/statue.glb` |
| Hero B | `…/amphora.glb` | `assets/museum_studio/amphora.glb` |
| Hero C | `…/bust.glb` | `assets/museum_studio/bust.glb` |
| Pedestal | `…/pedestal.glb` | `assets/museum_studio/pedestal.glb` |
| Floor ref | `…/env/floor_albedo.jpg` + `floor_normal.jpg` | `assets/museum_studio/` |
| Optional dress | `column.glb`, `arch.glb`, `tablet.glb` | same |
| Env | keep studio HDRI | existing `brown_photostudio_02_2k.hdr` |
| Relight | outdoor pure sky | existing |

## Concept stills (Imagine)

| ID | File | Hero |
|----|------|------|
| M1 | `M1_statue.jpg` | Classical statue |
| M2 | `M2_amphora.jpg` | Amphora / ceramic |
| M3 | `M3_bust.jpg` | Marble bust |
| M0 | `M0_floor_ref_edit.jpg` | Floor-texture-conditioned full shot |
| C1–C3 | earlier product concepts | helmet / boombox / spheres |

## Build table (locked draft for Phase 1)

| Slot | Spec |
|------|------|
| Cyclorama | Deep charcoal ~linear **(0.045, 0.048, 0.055)**, tall flat wall (curve optional later) |
| Floor free θ | Dense **albedo** first (+ ORM.g quality row); marble black/white tile GT |
| Floor visual GT | Soft-quantize `floor_albedo.jpg` to free-grid G×G (or procedural 4–8 tile marble); CI keeps harsh checker stress |
| Pedestal | Prefer `pedestal.glb`; fallback procedural box slate `(0.18,0.19,0.21)` |
| Hero | **`statue.glb`** (fixed textures). Secondary presets: amphora, bust later |
| Lights | Warm key left, cool rim, soft fill; studio HDRI scale modest |
| Cameras | Lower product angle; floor ≥40% frame; front / ¾ / opposite (`camDistMul` ~1.05) |
| GT pattern | `soft_marble_tiles` SHOW · `checker_lab` CI |
| Preset name | **`--preset museum`** (alias cinematic) |
| Optional dress | One `column.glb` or `tablet.glb` off-axis, **not** in free θ |

### Multi-view concept pack (Imagine)

| File | Role |
|------|------|
| `M1_front.jpg` | Locked front / base |
| `M1_three_quarter.jpg` | Orbit ~35° |
| `M1_opposite.jpg` | Opposite / profile |
| `M1_relight.jpg` | Novel light mood |
| `M1_wrong_init.jpg` | Floor wrong-init mock (before) |

## Engine constraints (unchanged)

- Floor atlas UVs [0,1]² for bindless dense SoT  
- Hero materials **not** free θ in v1  
- MAPTEST / quality-plate gates unchanged numerically  
- Do **not** claim Objects-with-Lighting / OpenIllumination parity  

## Phase 0–1 checklist

- [x] Lock hero: **M1 statue**  
- [x] Multi-view + relight + wrong-init Imagine edits  
- [x] Brief filled with build table  
- [x] Assets imported → `assets/museum_studio/`  
- [x] `--preset museum|museum_amphora|museum_bust|cinematic`  
- [x] Dark cyclorama + B&W marble tiles + mesh pedestal  
- [x] MAPTEST green on museum (256×144 dense albedo)  
- [x] README museum strip (`readme_museum.jpg`)  
- [ ] Optional 1080p SHOW polish (heavy NIUA mesh — slow)

## Phase 1 implementation sketch (after lock)

1. Copy glbs+textures → `assets/museum_studio/`  
2. `applyPreset("museum")` → model/pedestal paths + dark floor truth  
3. `buildStudio` polish: darker backdrop, optional pedestal mesh swap, marble soft GT for quality plate  
4. Quality plate + README figures rebuild  
5. Document as **OHAO museum studio protocol**
