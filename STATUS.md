# OHAO Engine — Status

> Single source of truth for "what actually works." Updated by running the
> examples and looking at the pixels, not by reading commit messages.

**Last verified:** after C++20 + golden unbreak + NRD YCoCg / deferred IBL fixes  
**Branch verified:** `master` (single trunk)  
**Build:** ✅ green — all 5 examples compile and link; pre-push goldens PASS

## Snapshot

- **Language:** C++20 (`CMAKE_CXX_STANDARD 20`)
- **Trunk:** `master` only (renovation / RT / DLSS / ReSTIR GI landed)
- **Goldens:** `tests/golden/` + `.githooks/pre-push` — cornell_box + metal_rough_spheres
- **PathTracer:** lazy-init offline/realtime profiles; scene re-uploaded on first create
- **model_viewer** default resolution **1920×1080** (4K dual-PT OOMs on 8 GiB)

## Feature matrix (evidence-based)

Legend: ✅ works · ⚠️ works with caveats · ❌ broken · 🧪 experimental · 🗑️ removed

| Feature | State | Evidence / Notes |
|---|---|---|
| **Build (all targets)** | ✅ | Clean compile + link |
| **Path tracer — Cornell box** | ✅ | Unbiased GI; golden PASS |
| **Path tracer — PBR (MetalRoughSpheres)** | ✅ | Golden PASS @ 16 spp |
| **Path tracer — env map (env_demo)** | ✅ | Helmet + HDRI; OIDN clean |
| **Path tracer — Fox.glb (model_viewer)** | ✅ | Was hang @ 4K; now 16–48 spp finishes in seconds @ 1080p |
| **Deferred — dielectrics / room** | ✅ | Walls, floor, soft shading OK |
| **Deferred — metallic + env** | ⚠️ | IBL flag + BRDF-LUT fallback enabled; equirect IBL is approximate (no prefilter cube / LUT). Still dimmer than PT room reflections |
| **Denoiser — OIDN** | ✅ | Default offline path |
| **Denoiser — OptiX** | 🗑️ | Removed; `optix` → OIDN fallback |
| **Denoiser — NRD** | ✅ | Was magenta (linear RGB fed as YCoCg). Fixed: pack YCoCg + norm hit-dist in raygen; unpack in `nrd_compose` |
| **DLSS-RR / ReSTIR GI** | ⚠️ | Interactive path; outdoor HDRI showcase works |
| **Turntable / interactive** | ✅ / ❓ | Turntable frames OK; interactive needs display |
| **Skeletal animation** | 🗑️ | Removed in renovation cleanup |
| **SSS** | 🧪 | Biased look-dev hacks; not a true BSSRDF |

## Bugs fixed (this pass)

### Fox.glb hang (was Bug #3)
- **Was:** model_viewer never finished at high spp / 4K.
- **Now:** 1 spp ~0.1 s, 48 spp ~3.5 s @ 1080p with valid pixels.
- **Cause:** dual PathTracer OOM + broken offline init at 4K; fixed by lazy PT + 1080p default.

### NRD magenta / pink cast
- **Was:** NRD beauty G/R ≈ 0.5 (purple helmet); raw pre-NRD AOVs neutral.
- **Root cause:** REBLUR expects **YCoCg + normalized hit-distance** (`NRD.hlsli` Pack/Unpack). We wrote linear RGB + raw world hit-dist.
- **Fix:** `shaders/includes/rt/nrd_frontend.glsl`; pack in offline/realtime raygen; unpack in `nrd_compose.comp`.

### Deferred metals pure black (partial)
- **Was:** IBL flag only when irradiance cube set; `setEnvMap()` alone never enabled IBL. BRDF LUT dummy zeroed split-sum.
- **Fix:** enable IBL when env map bound; analytical BRDF-LUT fallback + metal ambient floor in `deferred_lighting.frag`.
- **Remaining:** full `IblProcessor` prefilter + real BRDF LUT still not wired → PT room bounce still wins for chrome look.

## Known limitations (not open bugs)

1. **Deferred metals vs PT** — no multi-bounce room GI in deferred; equirect IBL is a floor, not parity with path tracing.
2. **SSS** — biased; keep out of “offline ground truth” claims.
3. **Cloud CI** — still no GPU-less build/unit workflow (Phase 0 leftover).
4. **Skinned RT BLAS** — animation subsystem removed; gap is moot until reintroduced.

## Inverse rendering (physical + Diff-IR)

| Item | State |
|------|--------|
| **Goal** | Recover scene params θ / maps so R(θ) ≈ target |
| **Backends** | `--backend pt` · `--backend diff` (Deferred map SoT) · `--backend hybrid` (Diff fit → PT lab eval) |
| **Lab plate (PT)** | Capture-gated holdout **32.5** / relight **34.4** / gain **+20.5** dB ✅ (`metric_gt=capture_export_images`) |
| **Diff-IR** | Full studio mesh Deferred; tile θ (DIFFTEST) + **H1 dense albedo** + **H2 ORM.g + ORM.b** (`--dense-map` / `--dense-orm` / `--dense-metal`, MAPTEST) ✅ |
| **Beauty contract** | Diff: dense albedo + ORM sampled by Deferred (`_albedo_0` / `_roughmetal_0`); free-grid θ painted to ≥64². PT: physical θ + maps. |
| **HD plates** | `--hd 720\|1080\|full720\|full1080` — FIT optim + SHOW stills (720p / 1080p) |
| **Quality plate (publish bar)** | `--quality-plate` — **1080p SHOW @20f**, map≥128, multi-view, hard presets; dB only count here for product face |
| **Docs / media** | `docs/inverse_lab.md`, **`docs/inverse_lab_roadmap.md`** (long-run plan), `docs/render_pipelines.md`, `docs/media/inverse/` |
| **Showcase** | `scripts/run_inverse_showcase.sh` · `run_inverse_gallery.sh` · `run_inverse_quality_plate.sh` · deck in `docs/media/inverse/` |

```bash
./scripts/run_inverse_showcase.sh
./build/inverse_fit --backend diff --preset lantern --quality draft --out-dir renders/diff_selftest
# Daily-realistic HD stills (FIT half-res optim + SHOW 720p/1080p plate)
./build/inverse_fit --backend diff --dense-orm --dense-map-res 64 --dense-grid 4 \
  --hd 720 --preset lantern --out-dir renders/diff_dense_orm_hd720
./build/inverse_fit --backend diff --dense-metal --dense-map-res 64 --dense-grid 2 \
  --hd 720 --preset lantern --out-dir renders/diff_dense_metal_hd720
# Multi-preset gallery wall (M3a): lantern + helmet + spheres
./scripts/run_inverse_gallery.sh --fast
OUT_ROOT=renders/inverse_gallery_hd720 HD=720 ./scripts/run_inverse_gallery.sh
# Publish face (persuasion bar): hard presets @ 1080p clean stills — use this for claims
./scripts/run_inverse_quality_plate.sh
# Ablation table (M3b)
PRESET=spheres ./scripts/run_inverse_ablation.sh
# H3 photo-proxy plate (domain-shifted multi-view; PHOTOTEST, not LABTEST theater)
./scripts/run_inverse_photo_plate.sh
# H4/M5a analytic albedo GRADCHECK + dense MAPTEST
./build/inverse_fit --backend diff --dense-map --dense-map-res 64 --dense-grid 8 \
  --fit-width 256 --fit-height 144 --preset lantern --out-dir renders/diff_dense_analytic
python3 tools/inverse_lab/test_dense_analytic.py renders/diff_dense_analytic
./build/inverse_fit --backend hybrid --preset lantern --quality draft \
  --lab-bundle renders/inverse_lab/lantern_frontier/capture \
  --out-dir renders/inverse_lab/lantern_hybrid
./build/inverse_fit --backend pt --lab-bundle renders/inverse_lab/lantern_frontier/capture \
  --quality draft --out-dir renders/inverse_lab/lantern_frontier_fit
```

## Next actions

**Inverse lab (see `docs/inverse_lab_roadmap.md`):** **H1–H4 ✅** — M5a GRADCHECK + M5b analytic residual+sparse optim (**≥10×** vs 3-pass FD estimate, MAPTEST).

1. Real phone/COLMAP capture (same PHOTOTEST).  
2. Optional pure reverse-mode Adam polish (beyond residual closed-form).  
3. Keep claims honest — hybrid analytic+sparse, not full autodiff.
