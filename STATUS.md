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

## Inverse rendering (physical, no ML yet)

| Item | State |
|------|--------|
| **Goal** | Recover scene params θ so R(θ) ≈ target image |
| **Default scene** | **studio**: DamagedHelmet + ground + HDRI + multi-view |
| **Optimizes** | Ground albedo RGB (warm clay truth vs cool wrong init) |
| **Multi-view** | 2–3 cameras; mean FIT loss |
| **Relight** | After fit: hot key light → `recovered_relight.png` |
| **CLI** | `./build/inverse_fit --selftest --scene studio --quality draft` |
| **SHOW** | OIDN grain-free; FIT always raw MC |
| **Docs** | `docs/inverse.md` |
| **Not yet** | ML, autodiff, multi-param materials, real photos |

```bash
./build/inverse_fit --selftest --scene studio --quality draft
# → target_*, init_show, recovered_*, *relight*, trajectory.json
```

## Next actions

1. Inverse Phase 2: roughness more params (roughness) + multi-view / lighting fit; optional Adam.
2. Expand golden corpus (env helmet, deferred cornell).
3. Wire `IblProcessor` → deferred for proper metals/IBL (if deferred stays).
4. Keep this file honest after each meaningful change.
