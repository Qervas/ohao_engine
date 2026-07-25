# Inverse Lab — long-run roadmap

**Status:** living plan (2026-07)  
**Owner:** inverse / multi-pipeline track  
**North star:** a GitHub-visible *material capture + inverse rendering lab* on one Vulkan host — Deferred for dense fit, path tracer as oracle bar, modular C++20, honest capture-gated metrics.

This document elongates the short phase sketch into a multi-horizon plan. Update **Measured** columns when plates land; never claim a milestone without a gate that can fail.

---

## 0. Why this plan exists

### What we already proved (do not re-litigate)

| Plate | Backend | What it means |
|-------|---------|----------------|
| Capture-gated lantern | `--backend pt` | Holdout ≥28 / relight ≥26 / gain ≥8 is real (`metric_gt=capture_export_images`) |
| Diff Deferred map SoT | `--backend diff` | Wrong-init tile → dense map → bindless GBuffer albedo; DIFFTEST |
| Hybrid transfer | `--backend hybrid` | Diff fit → PT light+soft-tile refine → full LABTEST achievable |

Systems story already rare for a solo engine: **one host, two image formations, one lab protocol**.

### What is still “not top lab yet”

- Spatial θ is still **coarse** (2×2 tiles → painted dense map, not free per-texel optim).  
- Gradients are **coordinate FD / staged FD**, not true dense reverse-mode.  
- Scene gallery is **narrow** (lantern-first).  
- Capture is still **synthetic** (engine export, not real photos).  
- Materials are mostly **albedo (+ coarse rough/metal)**, not full ORM/normal stacks.

### Hero claim (locked for this roadmap)

> **Vulkan multi-pipeline inverse lab:** recover dense materials under Deferred, refine and score under a path-tracer oracle, with capture-gated novel-view and relight metrics — modular C++20, open plate, no metric theater.

We do **not** claim: public-dataset SOTA, free geometry, free camera, single uncalibrated photo, or Mitsuba-3 autodiff parity.

---

## 1. Principles (non-negotiable)

1. **Train-only loss** — holdout and relight never enter the optim objective.  
2. **Capture-gated metrics** — LABTEST uses exported PNGs; live oracle is diagnostic only.  
3. **Honest beauty SoT** — if we say “map drives beauty,” the rasterizer must sample that map (or we document tile materials explicitly).  
4. **Stage free-sets** — albedo first, then ORM, then lights; joint soup last.  
5. **Modular code** — soft ~350 / hard 500 LOC per TU; wiring in `inverse_fit` stays thin.  
6. **One face per phase** — each phase ends in a still strip + JSON gate + README/STATUS note.  
7. **Fail loud** — DIFFTEST / LABTEST / HYBRID / MAPTEST either pass or block the claim.

---

## 2. Architecture (stable spine)

```
                    ┌─────────────────────┐
   Capture bundle ─►│  Lab protocol       │◄── holdout / relight PNGs
                    │  train-only loss    │
                    └─────────┬───────────┘
                              │
           ┌──────────────────┼──────────────────┐
           ▼                  ▼                  ▼
      --backend pt      --backend diff     --backend hybrid
      PT FD / staged    Deferred map SoT   Diff fit → PT refine
      full θ oracle     dense fit workhorse  transfer + oracle bar
           │                  │                  │
           └──────────────────┴──────────────────┘
                              │
                              ▼
                    θ / maps + stills + metrics JSON
```

| Role | Backend | Future |
|------|---------|--------|
| **Oracle / gold bar** | Path tracer | Stays; denser spp when publishing |
| **Dense fit workhorse** | Diff Deferred | Dense maps, analytic grads, Adam |
| **Transfer plate** | Hybrid | Diff materials + short PT light/BRDF refine |

---

## 3. Horizon map (years → seasons → sprints)

| Horizon | Timebox (indicative) | Outcome |
|---------|----------------------|---------|
| **H0 — Foundation** | Done | L0–L5 protocol + Diff SoT + hybrid LABTEST |
| **H1 — Dense materials** | M1a–c landed | Free dense albedo 64/128² + in-place upload |
| **H2 — Multi-channel + gallery** | M2–M3 landed | ORM.g/b ✅; quality 1080p plate ✅; gallery + ablation ✅ |
| **H3 — Real capture** | M4a–b photo_proxy ✅; M1 ingestion pipeline ✅ (no shoot yet) | Recipe + PHOTOTEST; ChArUco→metric-pose ingest (`tools/inverse_lab/photo_ingest.py`, `docs/h3_capture_guide.md`) validated on a synthetic rehearsal only — **real photos still not shot** |
| **H4 — Analytic albedo + FD speedup** | M5a–b landed — ⚠️ *not* differentiable rendering | Analytic ∂L/∂albedo for the linear `I≈A⊙S` case (lighting detached) + FD-check + sparse-FD ~6× vs full FD. Roughness/metallic/lights/PT remain **finite-difference**. |
| **H5 — Ambition options** | Later / optional | Neural priors, public benches, geometry joint |

Horizons stack; do not start H3 before H1 ships a dense plate. H4 can overlap late H2 if H1 is solid.

> **Honesty (2026-07 audit).** Everything above H3 is validated on **synthetic self-consistency** — the fit target is the engine's own render of a known θ, so high dB is expected and is *not* evidence of inverse rendering on the world. The subsystem is a **finite-difference optimizer** with one analytic albedo gradient; it is not "differentiable rendering" (that's Phase 5). The only milestone that can produce a real, defensible claim is **H3 real photos** — treat it as the gate, not a nice-to-have. See `STATUS.md` → Inverse rendering reality check.

---

## 4. Phase book (elongated)

Each phase has: **intent**, **in/out**, **work packages**, **gates**, **demo face**, **exit criteria**, **explicit non-goals**.

---

### Phase 0 — Foundation (complete)

**Intent.** Prove inverse is real on this engine with honest metrics and multi-pipeline hosting.

**Delivered.**

- Modular `ohao/inverse/*`, `ohao/render/diff/*`  
- Capture format `ohao_inverse_lab_capture` v1  
- `--backend pt | diff | hybrid`  
- README / STATUS / `docs/media/inverse/` face  
- Tests: `test_metrics_and_maps`, `test_diff_ir`, `test_hybrid`

**Exit.** Do not reopen unless a regression fails a gate.

---

### Phase 1 — Dense albedo under Deferred (next primary)

**Intent.** Kill the “four floor colors” demo. Recover a **free dense albedo map** that Deferred actually samples.

#### 1.1 Parameterization

| Item | Target |
|------|--------|
| Map resolution | Start **64×64**, then **128×128** (publish at 128 when stable) |
| Domain | Linear RGB, UV atlas on ground (extend to pedestal later) |
| Init | Gray / noise / wrong-init map (not GT warm start) |
| Free set v1 | Albedo map only; lights + rough/metal frozen or coarse |
| Free set v2 | Map + low-dim lights (optional mid-phase) |

#### 1.2 Optim

- Adam (or AdamW) on map texels; learning-rate schedule + clamp to [0,1]  
- Loss: multi-view MSE (train only); optional TV / bilateral prior (weak)  
- Gradients **v1:** sparse FD on random texel batches *or* analytic ∂L/∂albedo through deferred shade  
- Gradients **v2 (preferred before phase exit):** analytic map Jacobian + FD consistency check (rel err bound)

#### 1.3 Engineering packages

1. **Map θ storage** — `DiffAlbedoMap` as optim state; serialize init/recovered/GT PNGs  
2. **Fast map upload** — staging overwrite / ring buffers (kill unload-recreate thrash)  
3. **Beauty SoT path** — already bindless; ensure single ground plane UV density matches map  
4. **MAPTEST gates** — map MSE vs GT + wrong-init; beauty train PSNR; optional holdout under Deferred  
5. **Hero stills** — GT map | init | recovered | beauty before/after  

#### 1.4 Gates (Phase 1 exit)

| Gate | Threshold (draft lantern, revise once measured) |
|------|--------------------------------------------------|
| Wrong-init map MSE → recovered | clear drop (document ratio) |
| Train beauty PSNR vs Deferred GT | improve ≥ 2 dB from wrong-init |
| Holdout (Deferred or hybrid) | no collapse vs Phase 0 hybrid |
| FD / analytic check | rel err documented if analytic lands |
| Code | no 1k-LOC god files |

#### 1.5 Demo face

README strip: **map triple + beauty pair**. Caption: “Dense albedo inverse on Vulkan Deferred.”

#### 1.6 Non-goals for Phase 1

Normals, full BRDF lobes, real photos, public datasets, replacing PT.

---

### Phase 2 — Multi-channel materials (ORM / rough-metal)

**Intent.** Materials you can **relight**, not just recolor.

#### 2.1 Channels

| Channel | Priority | Notes |
|---------|----------|--------|
| Albedo RGB | P0 | From Phase 1 |
| Roughness | P0 | Map or coarse field |
| Metallic | P1 | Often sparse; strong prior OK |
| AO | P2 | Optional; easy to bake wrong |
| Normal | P3 | Defer unless mesh is dense enough |

#### 2.2 Free-set schedule

1. Freeze lights; optim albedo + roughness  
2. Unlock metallic with L2/sparsity prior  
3. Short hybrid PT light refine (reuse Phase 0 hybrid machinery)  
4. Joint refine with tiny LRs  

#### 2.3 Gates

| Gate | Threshold |
|------|-----------|
| Relight PSNR (capture-gated) | ≥ **26 dB** (or ≥ Phase 0 PT relight − 3 dB if harder maps) |
| Holdout PSNR | ≥ **28 dB** under hybrid or PT eval |
| Map channel MSE | albedo primary; ORM reported |
| Visual | no chrome floor / plastic mud without disclosure |

#### 2.4 Demo face

**Same camera, two HDRIs** — GT materials vs recovered materials. Optional short mp4.

#### 2.5 Non-goals

Measured BRDF databases, anisotropic metals, SSS inverse.

---

### Phase 3 — Gallery & protocol hardening

**Intent.** Prove the lab is a **system**, not a one-scene overfit.

#### 3.1 Scene matrix

| Preset | Why |
|--------|-----|
| lantern | Baseline plate |
| helmet / product metal | Specular stress |
| spheres / metal-rough chart | Known BRDF chart |
| outdoor / env-heavy | HDRI conditioning |
| (optional) boombox / toycar | Clutter / multi-material |

#### 3.2 Protocol upgrades

- Capture v1.1: document spp, denoise=none, map_res, seeds  
- `run_inverse_showcase.sh` → multi-preset matrix job  
- CI-ish local script: DIFFTEST + hybrid on lantern always; weekly gallery  
- Metric dashboard table in `docs/inverse_lab.md` (auto-updated snippet optional)

#### 3.3 Robustness stress

| Stress | Expectation |
|--------|-------------|
| Wrong-init severity | gray / hue-shift / noise maps |
| Fewer train views | 2 → 1 train ablation |
| Map resolution ladder | 32 / 64 / 128 tradeoff curve |
| Denoise mismatch | document; keep gate on raw export |

#### 3.4 Gates

- ≥ **3 presets** pass published thresholds (may be per-preset tables)  
- No preset silently uses live-oracle GT for LABTEST  
- Showcase script green on clean machine with assets  

#### 3.5 Demo face

**Gallery wall** — 3× (wrong / recovered / target) + map strips.

---

### Phase 4 — Real multi-view photo plate

**Intent.** Cross the sim-to-real line with **honesty**.

#### 4.1 Capture recipe (minimum viable lab)

| Item | Spec |
|------|------|
| Object | 1 diffuse-ish product + 1 slightly glossy |
| Views | 8–20 stills, overlap for pose |
| Pose | COLMAP or ChArUco board; publish cameras.jsonl |
| Mesh | Phone scan / existing GLB / simplified mesh — **fixed** at fit time |
| Lighting | Indoor + optional second HDRI for relight attempt |
| Calibration | White balance locked; exposure documented |

#### 4.2 Fit recipe

- Fix mesh + cameras (or small pose refine as stretch)  
- Free: dense albedo (+ rough); lights coarse  
- Eval: held-out views; optional “relight” as second session  

#### 4.3 Gates (looser than synthetic — document why)

| Gate | Guidance |
|------|----------|
| Holdout PSNR | Report absolute + gain vs wrong-init; **no fake ≥28** if unattainable |
| Visual | Side-by-side photo vs re-render |
| Failure cases | Specular blowouts, shadow errors, mesh holes — shown, not hidden |

#### 4.4 Demo face

**Photo → maps → re-render** hero on README. This is the “lab demo day” moment.

#### 4.5 Non-goals

Unposed casual selfie inverse; NeRF-style free camera field; city-scale.

---

### Phase 5 — True differentiability (Deferred first)

**Intent.** Stop paying O(n) FD for n texels. Earn “differentiable rendering” without lying.

#### 5.1 Scope order

1. **Deferred albedo:** analytic ∂L/∂albedo through local shade (GBuffer albedo path is almost linear in albedo under fixed lighting — exploit that)  
2. **Deferred rough/metal:** harder; finite-diff residual OK initially  
3. **One-bounce PT / shadow:** research spike; not phase exit  
4. **Full pathwise PT autodiff:** out of scope unless a year-scale bet  

#### 5.2 Validation

- FD check on random texels (rel err threshold)  
- Same optim quality vs Phase 1 FD baseline, **≥10×** wall-clock on 128²  
- No silent NaNs; clamp + finite checks  

#### 5.3 Gates

| Gate | Threshold |
|------|-----------|
| Grad consistency | median rel err &lt; ~5–10% on albedo |
| Speed | ≥10× vs naive full-map FD |
| Quality | Phase 1/2 quality held or improved |

#### 5.4 Demo face

Timing table + “analytic vs FD” agreement plot/stills.

---

### Phase 6 — Optional ambition tracks (only after H1–H3)

These are **branches**, not the trunk.

| Track | When | What |
|-------|------|------|
| **6A Neural material prior** | After dense maps | Network predicts init map / residual; always refine physically |
| **6B Public benchmarks** | After photo plate | DTU / synthetic NeRF-inverse style — only if mesh/cam story matches |
| **6C Joint pose** | After dense maps | Small SE(3) refine; careful gauge freedoms |
| **6D Geometry inverse** | Much later | Depth/normal; different product |
| **6E Interactive inverse** | After speed (H4) | Live Deferred fit slider / progressive maps |

Trunk remains: **dense physical maps + hybrid oracle + capture discipline**.

---

## 5. Milestone checklist (ship units)

| ID | Name | Horizon | Depends | Exit artifact |
|----|------|---------|---------|---------------|
| **M0** | Foundation plate | H0 | — | ✅ hybrid LABTEST + README face |
| **M1a** | Dense albedo 64² | H1 | M0 | ✅ MAPTEST + map triple stills (`--dense-map`) |
| **M1b** | Dense albedo 128² | H1 | M1a | ✅ MAPTEST at `--dense-map-res 128` |
| **M1c** | Fast map upload | H1 | M1a | ✅ `updateTextureFromMemory` in-place SoT |
| **M2a** | Roughness map | H2 | M1b | ✅ MAPTEST + synthetic relight (`--dense-orm`) |
| **M2b** | Metallic + priors | H2 | M2a | ✅ MAPTEST + relight (`--dense-metal`); HD 720/1080 plates |
| **M3a** | 3-preset gallery | H2 | M1b | ✅ lantern/helmet/spheres MAPTEST + `gallery_wall.html` |
| **M3b** | Ablation table | H2 | M3a | ✅ quality baseline + map/views/hd/lab_fast matrix |
| **M4a** | Photo capture recipe | H3 | M1b | ✅ `docs/inverse_photo_lab.md` + cameras.jsonl |
| **M4b** | Photo inverse plate | H3 | M4a, M2a | ✅ photo_proxy + PHOTOTEST + photo_vs_rerender strip |
| **M4c** | ChArUco photo ingest | H3 | M4a | ✅ tool + synthetic rehearsal gate (center RMSE 1.17 mm, rot ≤0.26°); ⏳ awaiting a real shoot |
| **M5a** | Analytic albedo grads | H4 | M1b | ✅ GRADCHECK (med rel err ~10–20% vs FD) |
| **M5b** | Analytic dense optim | H4 | M5a | ✅ linear solve + residual + sparse; ≥10× (~20×) vs 3-pass FD + MAPTEST |
| **M6*** | Optional tracks | H5 | M4b | Separate RFCs |

---

## 6. Metrics bible (how we speak in public)

### Always report

- `metric_gt` (`capture_export_images` vs live)  
- Train / holdout / relight PSNR + SSIM  
- Gain vs wrong-init  
- Map MSE (when maps exist)  
- Backend (`pt` / `diff` / `hybrid`)  
- Resolution, spp, free-set, init type  

### Never report as pass evidence

- Live-oracle PSNR alone  
- Train PSNR alone  
- Warm-start “recovery” without wrong-init baseline  
- Map PNG export without beauty SoT  

### Threshold evolution

| Regime | Holdout | Relight | Gain | Notes |
|--------|---------|---------|------|--------|
| Synthetic lantern PT oracle | ≥28 | ≥26 | ≥8 | Phase 0 locked |
| Dense map hybrid | ≥28 preferred | ≥26 | ≥8 | May add map MSE |
| Photo plate | report + gain | optional | ≥5–8 | Absolute PSNR may be lower — disclose |

---

## 7. Engineering backlog (cross-cutting)

| Item | Why | Phase |
|------|-----|-------|
| Map upload ring / persistent VkImage | Long Adam runs | 1 |
| Single dense ground mesh UV | Match map density | 1 |
| Analytic shade ∂/∂albedo | Speed + credibility | 5 |
| Showcase matrix script | Gallery | 3 |
| Capture v1.1 schema fields | Repro | 3–4 |
| LOC discipline + module splits | Art of the code | all |
| Golden path-tracer hook vs inverse | Unrelated; don’t block inverse claims | ops |

---

## 8. Risk register

| Risk | Symptom | Mitigation |
|------|---------|------------|
| Lighting–albedo ambiguity | Pretty train, bad relight | Freeze lights early; relight gate |
| Overfitting 2 views | High train, low holdout | Holdout gate always on |
| Map thrash / GPU stalls | Unload-recreate | Persistent image + staging |
| Specular pollution | Floor goes metal | Product priors; separate metal chart preset |
| Photo mesh holes | Black fringes | Mesh cleanup; mask loss |
| Scope creep to NeRF | Free cam / free geo | Non-goals list; RFC for M6 |
| Metric theater | Live GT in LABTEST | Code + docs forbid |

---

## 9. Cadence & governance

| Ritual | Frequency | Output |
|--------|-----------|--------|
| Plate run (lantern hybrid or dense) | Each meaningful inverse PR | metrics JSON in `renders/` (gitignored) + STATUS line |
| Roadmap touch | End of each milestone | Checkboxes / Measured columns |
| README face | Each phase exit | One new still strip max; retire stale claims |
| Scope RFC | Before any M6 track | Short doc: claim, gate, non-goal |

**Definition of done for a phase:** gates green **and** demo face landed **and** STATUS/roadmap updated.

---

## 10. Suggested calendar (indicative, not a contract)

```
Now ──────── H1 Dense albedo ──────── H2 ORM + gallery ──────── H3 Photo plate ──── H4 Autodiff
     M1a  M1b  M1c              M2a M2b  M3a M3b           M4a M4b              M5a M5b
     |---- ~4–8 w ----|         |---- ~4–6 w ----|        |---- ~6–10 w ----|   |---- ~6–12 w ----|
```

Adjust with hardware time (PT refine is the wall-clock hog). Prefer **shipping M1a** over polishing hybrid another 1 dB.

---

## 11. Immediate next actions

1. **M1a–c ✅** — dense map 64/128², free grid, in-place upload, MAPTEST.  
2. **M2a ✅** — free dense roughness (ORM.g) under Deferred; floor-crop specular loss; synthetic key-light relight gate.  
3. **M2b ✅** — free dense metallic (ORM.b); extreme-flip FD; `--hd 720|1080` daily plate sizes.  
4. **M3a ✅** — multi-preset gallery wall (`scripts/run_inverse_gallery.sh`; lantern/helmet/spheres).  
5. **M3b ✅** — ablation table + **quality plate bar** (`--quality-plate`, hard presets @1080p).  
6. **M4a–b ✅** — photo recipe + photo_proxy plate + PHOTOTEST (gain-based, no fake ≥28).  
7. **M5a–b ✅** — analytic GRADCHECK + residual/sparse optim (**≥10×** ~20× vs full FD, MAPTEST).  
8. **Next** — real_photo COLMAP (same PHOTOTEST) or optional reverse-mode polish.

---

## 12. References (internal)

| Doc | Role |
|-----|------|
| `docs/inverse_lab.md` | Protocol + current thresholds |
| `docs/render_pipelines.md` | Backend wiring |
| `docs/inverse.md` | Product overview |
| `STATUS.md` | What actually works |
| `docs/media/inverse/` | Public stills / deck |
| `scripts/run_inverse_showcase.sh` | One-shot plate |

---

## 13. One-line mantra

**Dense maps under Deferred. Truth under the path tracer. Gates on capture. No theater.**
