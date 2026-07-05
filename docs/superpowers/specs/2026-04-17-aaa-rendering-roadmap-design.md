# AAA Rendering Roadmap — Design

**Date:** 2026-04-17
**Status:** Approved design, pending implementation plan
**Scope:** Top-level rendering roadmap for OHAO engine to reach AAA tier (UE5 realtime + Cycles offline parity)

---

## 1. Goal

Build OHAO's rendering subsystem to top-tier quality. Single codebase, two output modes:

- **Offline mode** — competitive with Blender Cycles for film-quality stills and rendered video
- **Realtime mode** — competitive with Unreal Engine 5 for interactive game applications

This is the foundation for later engine work (gameplay, editor, scripting, networking, AI). Rendering ships first.

## 2. Non-Goals

Explicitly out of scope for this roadmap:

- Gameplay systems, scripting, editor tooling, networking, AI features — deferred until rendering ships
- Niche BRDFs (thin-film interference, anisotropic except for hair) — unless required by a listed feature
- New asset formats beyond GLTF / FBX / OBJ (already supported)

## 3. Principles

### 3.1 Quality > Speed

Every feature passes through a verification loop before it's considered done:

```
build → test → visual comparison vs reference → commit
```

No feature bundling. No "ship now, polish later." If it doesn't match or beat the reference, it's not done.

### 3.2 Offline-First

Offline is the oracle. Realtime denoisers need ground truth to validate against. Offline renderer matures before realtime denoiser infrastructure begins.

### 3.3 Reference-Driven Development

Every rendering feature has a named reference implementation or scene it must match:

- **Cornell box variants** — unit-level correctness tests
- **Curated Blender scenes** — Bistro, Sponza, Classroom, etc; community-recognized benchmarks
- **Custom test suite** — 5–10 scenes stress-testing specific features (skin, fog, fabric, metal, glass, hair)

A feature is complete when its reference render matches or beats the comparison target.

## 4. Phase Structure

Four phases. Strict order. Each phase has a hard exit criterion.

### Phase 1 — Offline Cycles-class

Close the gap between current offline RT (~75–85% of Cycles quality) and full Cycles parity on typical production scenes.

Features:

| ID  | Feature                          | Reference target                  |
|-----|----------------------------------|-----------------------------------|
| 1.1 | MIS env + BSDF                   | Cycles env-lit turntable          |
| 1.2 | Blue noise / Owen-scrambled Sobol sampler | Variance curve at N samples |
| 1.3 | SSS burley                       | Cycles skin shader                |
| 1.4 | SSS random-walk                  | Wax / jade / marble scene         |
| 1.5 | Adaptive sampling (noise threshold stop) | Adaptive Cycles scene        |
| 1.6 | Firefly clamp                    | High-contrast interior            |
| 1.7 | Homogeneous volumetrics          | Fog-filled room                   |
| 1.8 | Heterogeneous volumetrics        | Smoke plume                       |

**Exit criterion:** at equal sample count, OHAO offline renderer matches or beats Cycles on a character + room scene suite.

### Phase 2 — Realtime Denoiser Infrastructure

Current realtime RT is ~35–45%. Blocker is denoising: raw RT radiance → perceptually clean image under motion. This phase builds the denoiser stack from scratch.

Features:

| ID  | Feature                                            | Reference target                  |
|-----|----------------------------------------------------|-----------------------------------|
| 2.1 | Motion vector audit (camera + skeletal animation)  | MV debug view correctness         |
| 2.2 | Pass split: raw RT → temporal → spatial → resolve  | Each pass visualizable            |
| 2.3 | Temporal reprojection                              | History accumulation stability    |
| 2.4 | Variance / moments estimation                      | Moments debug view                |
| 2.5 | Spatial filter (edge-preserving, variance-guided)  | Edge preservation test            |
| 2.6 | Disocclusion masks                                 | Fast camera-move stress scene     |
| 2.7 | SVGF or ReBLUR-class full integration              | 1spp RT GI clean under motion     |

**Exit criterion:** 1 sample-per-pixel ray-traced GI produces a perceptually clean image under both camera motion and skeletal animation.

### Phase 3 — AAA Signature Features

Bring the engine from "correct" to "industry-visible." These are the features that make demo scenes impressive.

Features:

| ID  | Feature                                    | Notes                                         |
|-----|--------------------------------------------|-----------------------------------------------|
| 3.1 | Virtual shadow maps                        | UE5 VSM-style: sparse, high-res, streamed     |
| 3.2 | Lumen-class hybrid GI                      | RT + probe cache fallback                     |
| 3.3 | Atmosphere (Hillaire sky model)            | Physically-based atmospheric scattering       |
| 3.4 | Hair / fur BRDF                            | Marschner or Chiang                           |
| 3.5 | Nanite-equivalent (cluster + software raster) | **Optional monster feature**               |

**Exit criterion:** scenes in OHAO demo like UE5's Valley of the Ancient tech demo.

### Phase 4 — Production Pipeline

Polish and feature parity with shipping engines.

Features:

| ID  | Feature                                      |
|-----|----------------------------------------------|
| 4.1 | Post-process stack (DoF, motion blur, chromatic aberration, film grain) |
| 4.2 | DLSS / FSR / XeSS upscaling                  |
| 4.3 | GPU particle system                          |
| 4.4 | Decals                                       |

**Exit criterion:** engine is ship-ready for external use.

## 5. Verification Methodology

### 5.1 Per-Feature Loop

For every numbered feature (1.1 through 4.4):

1. **Design** — brief spec; inputs, outputs, reference target
2. **Build** — implement in isolation; don't touch unrelated code
3. **Unit test** — where feature is testable without rendering (sampler distributions, NDF sampling, etc.)
4. **Render comparison** — before/after screenshots on the reference scene
5. **Cross-check** — side-by-side against reference (Cycles for offline, UE5/RTXDI for realtime where possible)
6. **Gate** — commit only when feature matches or beats reference
7. **Next feature**

### 5.2 Reference Scene Library

Maintained in `tests/reference_scenes/`:

- `cornell/` — controlled unit tests (material, light, BRDF correctness)
- `community/` — Blender community scenes (Bistro, Sponza, Classroom, Lone Monk)
- `custom/` — feature regression suite (skin_closeup, fog_interior, fabric_drape, metal_scratch, glass_caustic, hair_portrait)

Each scene has:

- Source file (GLB/FBX/Blender)
- Reference render (PNG, known-good)
- Feature tags (which features the scene validates)
- Camera + lighting preset (deterministic)

### 5.3 Regression Policy

Every completed feature adds a reference render to `tests/reference_scenes/`. Future changes that break a reference render require explicit buy-in before merging.

## 6. Timeline

No hard calendar commitment. Quality gate is the pacing mechanism.

Indicative ordering (not deadlines):

- Phase 1: ~2–3 months at ~1 feature per 1–2 weeks
- Phase 2: ~2–3 months
- Phase 3: ~3–4 months (Nanite alone could extend this)
- Phase 4: ~2 months

Total range: roughly 9–12 months full-time-equivalent effort. Longer if work is part-time.

## 7. Risks

| Risk                                              | Mitigation                                        |
|---------------------------------------------------|---------------------------------------------------|
| Scope creep into gameplay/editor/AI               | Explicit non-goals (section 2); defer with memory |
| Over-engineering early features (YAGNI violation) | Per-feature reference target forces minimum scope |
| Realtime denoiser work before offline is stable   | Phase gate: offline exits before realtime starts  |
| Nanite-equivalent (3.5) consuming entire timeline | Flagged optional; can be dropped or deferred      |
| Reference benchmarks drift over time              | Each feature commits its reference render         |

## 8. Out-of-Scope Work (Parking Lot)

Tracked here so it doesn't get lost, but explicitly deferred until rendering ships:

- Gameplay: OhaoCharacter, damage system, inventory, save/load
- Editor: scene tool, material editor, animation timeline
- Networking: replication, rollback netcode
- Scripting: Lua / C# / other
- AI: MCP expansion, AI scene editor (OpenClaw-style)
- Niche BRDFs: thin-film, non-hair anisotropic, clearcoat (unless hair BRDF shares infrastructure)
- Asset streaming / virtualized texture system (revisit if Phase 3.5 Nanite is done)

## 9. Next Step

Invoke `superpowers:writing-plans` to produce a detailed implementation plan for **Phase 1, Feature 1.1 (MIS env + BSDF)** — the first feature to execute.
