# OHAO Engine — Status

> Single source of truth for "what actually works." Updated by running the
> examples and looking at the pixels, not by reading commit messages.

**Last verified:** 2026-06-24
**Branch verified:** `animation` (227 commits ahead of `master` — see Integration Debt)
**Build:** ✅ green — all 5 examples + `engine_tests` compile and link (`cmake --build build -j8`, exit 0)

## Renovation log — 2026-06-24 (subsystem removals)

Pre-renovation cleanup. Each removed independently, rebuilt + render-verified green:

- ❌ **Skeletal animation** — removed entirely (`ohao/animation/`, RT gpu-skinning + animated-RT manager, skinned GBuffer/CSM pipelines, `*_skinned.vert`, ~18 consumer files unwired). Static rendering verified correct (MetalRoughSpheres PT conformance unchanged). Inert `boneIndices`/`boneWeights` left in `Vertex` (stride unchanged, low-risk).
- ❌ **OptiX denoiser** — removed (was segfaulting on shutdown). `--denoise=optix` now degrades gracefully to OIDN.
- ❌ **Godot `.tscn` loader** — removed (superseded by external godot-mcp work).
- ❌ **Scene serialization** — removed (`SceneSerializer` + 5 component serialize/deserialize stubs + dead test). All were never-called stubs. Revisit with `.ohao` format later.
- ✅ **Audio** — KEPT (per decision; small, harmless).

Direction: re-architect toward a **differentiable, inverse-rendering, offline-first** engine (pbrt-style `.ohao` scene format, Slang shaders, one unified integrator). Net-before-surgery sequencing. These removals are step 1 (delete accidental/half-baked complexity).

### Phase 0 — deterministic render contract + golden net (in progress, 2026-06-25)
Spec: `docs/superpowers/specs/2026-06-24-renovation-phase0-foundation-design.md`
- ✅ **Determinism fixed.** Root-caused the "3 runs → 3 hashes" non-determinism to a cross-invocation read-after-write race: the offline raygen's temporal reprojection read `accumBuffer` at a reprojected neighbor pixel that sibling threads concurrently write. Disabled reprojection for the static offline path. Now bit-identical run-to-run except an irreducible ~6px@1LSB GPU floating-point floor (averaged to 0 at compare-res).
- ✅ **Golden harness** `tests/golden/render_golden.py` + `manifest.json` + downscaled goldens (~450 KB). Tolerance compare. Validated: passes vs goldens, self-test deterministic, catches a wrong-spp regression (exit 1).
- ✅ **Pre-push hook** `.githooks/pre-push` (install: `git config core.hooksPath .githooks`). Cloud CI (build/unit/lint, no GPU) and trunk cleanup still TODO.

---

## Feature matrix (evidence-based)

Legend: ✅ works · ⚠️ works with caveats · ❌ broken · 🧪 experimental/biased · ❓ untested

| Feature | State | Evidence / Notes |
|---|---|---|
| **Build (all targets)** | ✅ | Clean compile + link, exit 0 |
| **Path tracer — Cornell box** | ✅ | Correct unbiased GI, color bleeding, glass + metal spheres (64 spp) |
| **Path tracer — PBR material model** | ✅ | MetalRoughSpheres conformance render correct: metal/dielectric + roughness gradient reproduced |
| **Path tracer — env map (env_demo)** | ⚠️ | Helmet renders correct metal/visor; studio HDR backdrop reads flat — verify exposure/compositing |
| **Deferred — simple/untextured (Cornell)** | ✅ | Red/green walls, spheres, soft shading all correct |
| **Deferred — metallic surfaces** | ❌ | **Metals crush to black** (DamagedHelmet, BoomBox). Dielectric/textured parts render fine. Bug #1 below. |
| **Subsurface scattering (SSS)** | 🧪 | Biased: albedo-warmth gating + vertex-normal curvature proxy + Gaussian wrap. Tuned per-portrait, not a true BSSRDF. Violates "offline stays unbiased." |
| **Denoiser — OIDN** | ✅ | Default everywhere; clean output across all PT renders |
| **Denoiser — OptiX** | 🗑️ removed | Was: correct output but segfaulted on shutdown. Removed 2026-06-24; `optix`→`oidn` fallback. |
| **Denoiser — NRD (realtime)** | ⚠️ | Runs; output dim + magenta/pink shadow cast (AgX outset + surface-only compose). Metals also dark (same as Bug #1). |
| **Turntable video frames** | ✅ | Mirror Lantern in HDR sky, frames correct, saved to `renders/turntable/` |
| **model_viewer + Fox.glb (RTOffline)** | ❌ | **Hangs** — builds 11 BLAS then never finishes render in 280s at 48spp. Bug #3. |
| **Interactive viewer (GLFW)** | ❓ | `DISPLAY=:0` present but it's an interactive loop; can't meaningfully headless-test |
| **FBX / skeletal animation** | 🗑️ removed | Removed 2026-06-24 (was broken). Static FBX/GLB geometry loading retained via Assimp/ufbx. |

Evidence images for this pass live in the session scratchpad (not committed):
`cornell_pt.png`, `mv_pt.png`, `env_pt.png`, `cornell_deferred.png`, `mv_deferred.png`, `def_boombox.png`, `nrd_helmet.png`, `optix_helmet.png`, `renders/turntable/env_*.png`.

---

## Known bugs (concrete, with root cause)

### 1. ⚠️ Metals look near-black in deferred mode — KNOWN LIMITATION (not a code bug)
- **Repro:** `./build/model_viewer assets/showcase_objects/BoomBox.glb out.png 1 deferred`
- **Symptom:** metallic body is near-black except direct specular highlights; dielectric/textured parts fine.
- **Real cause:** a metal's appearance IS its reflection. In deferred with **no env map and no SSR/RT reflections, there is nothing in the scene to reflect** — only a flat ambient constant. The path tracer is unaffected because it bounces real rays off the Cornell walls. This is a *fundamental limitation* of deferred-without-reflections, not a defect.
- **Applied (Codex, 2026-06-24):** Fresnel-weighted flat-ambient fallback for metals when no env map is bound — lifts pure-black → dark-gray + faint wall reflections. Marginal; no regression to the env-present path. Kept.
- **Real resolution:** the unified integrator (renovation Phase 2+) path-traces and reflects the room correctly. Do NOT invest further in the frozen deferred pipeline (would need a default env map or SSR — polishing code slated for replacement).

### Subsystem disposition (renovation, decided 2026-06-24)
- **Deferred raster pipeline (`ohao/render/deferred/`)** — KEEP, frozen. It works; not deleted because the unified integrator that would replace it doesn't exist yet ("delete on replacement, not intention"). Bug #1 above handed to Codex.
- **Physics (`ohao/physics/`)** — quarantine (orthogonal, works; not in the offline-differentiable critical path).

### 2. ⚠️ OptiX denoiser segfaults on shutdown
- **Repro:** `./build/env_demo <model> <hdr> out.png 16 --denoise=optix`
- **Symptom:** renders and saves correct denoised PNG, prints `[Jolt] Backend shut down`, then **core dumps** (exit 139).
- **Likely cause:** teardown ordering — CUDA/OptiX resources freed after/around VkDevice or Jolt shutdown. Matches CLAUDE.md's documented "Shutdown order" class of bug.
- **Fix direction:** audit OptiX denoiser destructor vs. device/Jolt teardown order. Output is fine; only exit is unsafe.

### 3. ❌ model_viewer hangs on Fox.glb (RTOffline)
- **Repro:** `./build/model_viewer assets/test_models/Fox.glb out.png 48`
- **Symptom:** loads scene (11 BLAS, bedroom set + model), prints `Rendering (RTOffline)...`, never completes in 280s at 48 spp for a small model.
- **Status:** unconfirmed whether true hang (deadlock) or pathological slowness. Needs a single-spp + timed probe to localize.
- **Fix direction:** investigate during the system fix — could be animated-mesh BLAS path, a degenerate light, or an infinite bounce loop.

---

## Integration debt (the thing that makes it *feel* broken)

The code architecture is fine — the connective tissue isn't:

- `animation` is **227 commits ahead of `master`.** Every recent feature (denoisers, SSS, cinematic, DoF) is stranded here; `master` is frozen at a `chore:` commit.
- **5 live branches** diverged: `animation`, `dev`, `feat`, `scene_switching_fix`, `master`. No single source of truth in git.
- **Tuning-driven development with no "done" bar:** `SSS v2→v5`, `cinematic v3→v6`, `DoF v2→v3`. Completion signal is "looks better on one face," so progress became unmeasurable.

---

## Next actions (suggested order)

1. **Fix metals-in-deferred** (highest value, well-scoped, isolated) — wire IBL specular into the deferred metallic term.
2. **Decide SSS:** rip the biased hacks out of the offline path, or commit to a real BSSRDF. Keep look-dev hacks in *deferred*, not the reference renderer.
3. **Branch reconciliation:** merge `animation` → `master`, audit/kill `dev`/`feat`/`scene_switching_fix`. Get back to one trunk.
4. **Keep this file honest:** re-run the example sweep + update the matrix after each meaningful change.
