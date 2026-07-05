# Denoiser Sub-plan 3.D: Hit-Distance Packing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pack per-pixel hit-distance into the alpha channel of bindings 22 (diffuse radiance) and 23 (specular radiance) so NRD REBLUR can size its spatial filter per-pixel. Diffuse = single-segment (bounce-1 hit distance); specular = accumulated along specular chain with strict break on first diffuse sample at bounce ≥2.

**Architecture:** Shader-only sub-plan. All three raygens receive identical edits (no env-MIS divergence affects hit-distance logic). New per-pixel locals `specHitDist` / `specChainActive` / `diffHitDist` / `diffHitRecorded`. Stage B accumulates specular hit-distance each iteration while flag is true; flag flips on first diffuse BSDF sample at bounce ≥2. Stage C captures bounce-1 hit-distance via a latch. Exit writes pack hit-distance into alpha of bindings 22/23 instead of the current `1.0`.

**Tech Stack:** Vulkan 1.3 · GLSL ray tracing · no new bindings, no new images, no C++ changes.

**Reference spec:** `docs/superpowers/specs/2026-04-20-denoiser-subplan3d-hit-distance-packing-design.md`

---

## File Structure

**New files:** none.

**Modified files:**

| Path | Change |
|------|--------|
| `shaders/rt/pt_raygen.rgen` | Declare 4 new locals (specHitDist, specChainActive, diffHitDist, diffHitRecorded). Stage B inner loop: accumulate payload.hitDist while specChainActive. Stage B bounce-≥2 BSDF diffuse branch: set specChainActive = false. Stage C inner loop: latch payload.hitDist on first bounce via diffHitRecorded. Replace exit-write alpha `1.0` with hit-distance values. |
| `shaders/rt/pt_raygen_offline.rgen` | Identical edits (verbatim mirror; hit-dist logic is env-MIS-independent). |
| `shaders/rt/pt_raygen_realtime.rgen` | Identical edits — realtime's env-MIS absence doesn't affect this sub-plan. |
| `examples/env_demo.cpp` | `--dump-hit-dist-diffuse=<path>` + `--dump-hit-dist-specular=<path>` CLI flags. Extracts alpha channel from existing RGBA32F readbacks (`readbackDiffuseRadiance` / `readbackSpecularRadiance`), normalizes to max finite, writes grayscale PNG. |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.D entry with helmet hit-distance dump observations + max values. |

**No C++ core changes.** No descriptor bindings touched. No readback helpers added — alpha channel is already in the float vector that existing readbacks return.

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-3d-hitdist -b denoiser-3d-hit-distance-packing HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-3d-hitdist`.

Build bootstrap if the worktree's `build/` is empty:
```bash
cd /home/frankyin/Desktop/Github/ohao-3d-hitdist
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON 2>&1 | tail -10
# If build/_deps/glm-src/ is empty:
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/. build/_deps/
```

OptiX (optional):
```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: Shader hit-distance packing (all 3 raygens atomically)

**Files:**
- Modify: `shaders/rt/pt_raygen.rgen`
- Modify: `shaders/rt/pt_raygen_offline.rgen`
- Modify: `shaders/rt/pt_raygen_realtime.rgen`

### Step 1.1: Read pt_raygen.rgen structure

Identify the exact sites where edits need to land. Key landmarks in post-3.C.7 state (line numbers approximate):

- **Sub-plan 3.C accumulator block** (~lines 117-125): current locals `diffContrib`, `specContrib`, `firstHitDiffAlbedo`, `firstHitSpecColor`. The 4 new hit-dist locals go here.
- **Stage B init block** (~lines 430-470): `specDir`/`specThroughput`/`specLastBsdfPdf`/`specLastBounceWasDelta` set up. Stage B inner loop begins at `for (uint bounce = 1u; bounce <= maxBounces; bounce++) {` (~line 475).
- **Stage B inner loop — trace** (~line 476-479): `traceRayEXT(...)` fills payload.
- **Stage B miss branch** (~line 481-489): `if (payload.hitDist < 0.0) { ... break; }`.
- **Stage B bounce-≥2 BSDF sample** (~lines 695-750): the `if (bsdfChoice < specProb || roughness < 0.05) { ... } else { ... }` blocks. The diffuse branch (`else {`) is where `specChainActive = false` goes.
- **Stage C init block** (~lines 765-775): diffuse initial sample.
- **Stage C inner loop — trace** (~lines 778-782): same shape as Stage B.
- **Exit writes** (~lines 929-932 in post-3.C.7 state): `imageStore(diffuseRadiance, pixel, vec4(diffContrib, 1.0));` and `imageStore(specularRadiance, pixel, vec4(specContrib, 1.0));`.

Confirm these landmarks match before editing.

### Step 1.2: Add hit-distance locals to pt_raygen.rgen

Find the existing Sub-plan 3.C accumulator block. It declares:

```glsl
    // Sub-plan 3.C: diffuse + specular radiance split (demodulated)
    vec3 diffContrib        = vec3(0.0);
    vec3 specContrib        = vec3(0.0);
    vec3 firstHitDiffAlbedo = vec3(0.0);
    vec3 firstHitSpecColor  = vec3(0.04);
```

Immediately AFTER those 4 lines (before the Sub-plan 3.C.6 zero-init imageStore lines), add:

```glsl
    // Sub-plan 3.D: hit-distance per NRD REBLUR convention
    float specHitDist      = 0.0;   // accumulated along specular chain
    bool  specChainActive  = true;  // true until first diffuse sample at bounce >=2 breaks the chain
    float diffHitDist      = 0.0;   // single-segment: first secondary hit
    bool  diffHitRecorded  = false; // latch set once on first Stage-C bounce-1 hit
```

### Step 1.3: Add specular-chain accumulation in Stage B inner loop

Find the Stage B inner loop's `traceRayEXT(...)` call. Immediately AFTER that `traceRayEXT` line, BEFORE the `if (payload.hitDist < 0.0) { ... break; }` miss branch, add:

```glsl
            // Sub-plan 3.D: accumulate specular hit-distance while the specular chain is unbroken
            if (payload.hitDist >= 0.0 && specChainActive) {
                specHitDist += payload.hitDist;
            }
```

### Step 1.4: Break specular chain on diffuse BSDF sample at bounce ≥2

Find the Stage B bounce-≥2 BSDF-sample block. It contains a top-level branch:

```glsl
            if (bsdfChoice < specProb || roughness < 0.05) {
                // ... specular sample branch (updates specRayDir, specThroughput, MIS state)
            } else {
                // diffuse sample branch
                // ... (cosineHemisphere, update specThroughput for diffuse, etc.)
            }
```

Inside the `else { ... }` diffuse-sample branch, at the top of the block (first statement after `else {`), add:

```glsl
                // Sub-plan 3.D: specular chain broken by a diffuse bounce; stop accumulating hit-distance
                specChainActive = false;
```

### Step 1.5: Latch diffuse hit-distance in Stage C inner loop

Find the Stage C inner loop's `traceRayEXT(...)` call. Immediately AFTER the traceRayEXT, BEFORE the miss branch, add:

```glsl
            // Sub-plan 3.D: capture first secondary diffuse hit distance (single segment)
            if (!diffHitRecorded && payload.hitDist >= 0.0) {
                diffHitDist = payload.hitDist;
                diffHitRecorded = true;
            }
```

### Step 1.6: Update exit writes to pack hit-distance in alpha

Find the existing Sub-plan 3.C.6 exit-write block:

```glsl
    // Sub-plan 3.C.6: write RAW radiance; NRD remodulates using bindings 24/25.
    imageStore(diffuseRadiance,  pixel, vec4(diffContrib, 1.0));
    imageStore(specularRadiance, pixel, vec4(specContrib, 1.0));
```

Replace with:

```glsl
    // Sub-plan 3.C.6 + 3.D: RAW radiance in RGB, hit-distance in alpha (for NRD REBLUR filter sizing)
    imageStore(diffuseRadiance,  pixel, vec4(diffContrib, diffHitDist));
    imageStore(specularRadiance, pixel, vec4(specContrib, specHitDist));
```

### Step 1.7: Mirror edits to pt_raygen_offline.rgen

`pt_raygen_offline.rgen` is a verbatim copy of `pt_raygen.rgen` except the top 3-4 line comment block. Apply Steps 1.2-1.6 identically at the same logical sites.

Easiest approach: after editing `pt_raygen.rgen`, use `cp` then restore the top comment:

```bash
cd /home/frankyin/Desktop/Github/ohao-3d-hitdist
# Save offline's current top comment (lines 5-8 per precedent)
sed -n '5,8p' shaders/rt/pt_raygen_offline.rgen > /tmp/offline_header.txt
cat /tmp/offline_header.txt  # verify it's the "Offline / reference raygen shader..." comment
# Copy main from default
cp shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen
# Replace lines 5-6 (default's 2-line comment) with offline's 4-line comment
# Use Edit tool to swap the top comment block after cp
```

Verify parity:
```bash
diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen | head
```

Expected: only the top comment block differs (lines 5-8 vs 5-6).

### Step 1.8: Mirror edits to pt_raygen_realtime.rgen

Realtime raygen has the same structural landmarks (Stage A/B/C layout from 3.C.7, no env-MIS block). Apply Steps 1.2-1.6 at the analogous sites. Realtime doesn't have `specLastBsdfPdf`/`diffLastBsdfPdf` locals (those were env-MIS-only), but that's irrelevant here — the hit-dist locals are new additions, not modifications of MIS state.

### Step 1.9: Build shaders + full app

```bash
cd /home/frankyin/Desktop/Github/ohao-3d-hitdist
cmake --build build --target shaders -j8 2>&1 | tail -10
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean shader compile, clean link.

### Step 1.10: Beauty regression smoke — bit-identical expected

Beauty shouldn't change — we're only writing to the alpha channel of AOVs, never read by `radiance`.

```bash
./build/cornell_box /tmp/t1_3d_cornell.png 64 --denoise=none 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t1_3d_helmet.png 64 --denoise=none 2>&1 | tail -3
```

Expected: clean renders. Beauty visually identical to pre-3.D output. Use the Read tool on both PNGs to confirm. If beauty differs visibly, something leaked — investigate.

### Step 1.11: Quick alpha-channel sanity via existing readback

While the full CLI dump lives in Task 2, quickly verify alpha is non-zero for hit pixels:

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t1_3d_helmet_check.png 16 --denoise=none \
    --dump-diffuse=/tmp/t1_3d_diffuse_rgb_only.png \
    --dump-specular=/tmp/t1_3d_specular_rgb_only.png 2>&1 | tail -5
```

These dumps drop alpha (as current code does), so they'll render as before — but verify the build path exercised the new alpha writes without crashing.

If you want a quick alpha peek during T1: add a printf (temp, revert before commit) inside Stage B/C that logs the final hit-distance for a center pixel. Not required — T2 adds the proper alpha-dump path.

### Step 1.12: Commit

```bash
git add shaders/rt/pt_raygen.rgen \
        shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): pack hit-distance in radiance AOV alpha (Sub-plan 3.D)

All three raygens declare specHitDist/specChainActive + diffHitDist/diffHitRecorded
locals. Stage B accumulates payload.hitDist each bounce while specChainActive
is true; flag flips false on the first diffuse BSDF sample at bounce >=2. Stage C
captures bounce-1 hit-distance via a latch.

Exit writes at bindings 22 and 23 now pack hit-distance into the alpha channel:
  vec4(diffContrib, diffHitDist) and vec4(specContrib, specHitDist)

NRD REBLUR consumes alpha for per-pixel spatial-filter sizing. Beauty path
unchanged (radiance never reads alpha). Raw world-space units; normalization
lives in Sub-plan 4's NRD integration boundary via
REBLUR_FrontEnd_PackRadianceAndNormHitDist.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 2: env_demo hit-distance dump CLI + verification

**Files:**
- Modify: `examples/env_demo.cpp`
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

### Step 2.1: Add CLI flags + hit-distance dump lambda

Find the existing CLI parse loop in `examples/env_demo.cpp` (around the existing `--dump-spec-color` branch). Add two more branches:

```cpp
    std::string dumpHitDistDiffusePath;
    std::string dumpHitDistSpecularPath;
    // ... inside the existing for loop, add: ...
        else if (arg.rfind("--dump-hit-dist-diffuse=", 0) == 0) {
            dumpHitDistDiffusePath = arg.substr(24);
        } else if (arg.rfind("--dump-hit-dist-specular=", 0) == 0) {
            dumpHitDistSpecularPath = arg.substr(25);
        }
```

Near the top of `main()` alongside existing `dumpRGBA32FStream` / `dumpRGBA8ToRGB` helpers, add:

```cpp
    auto dumpHitDistance = [&](const std::string& path, const std::vector<float>& data,
                               uint32_t w, uint32_t h) {
        // Alpha channel of RGBA32F carries raw world-space hit-distance.
        // Normalize to max finite for 8-bit grayscale preview.
        float maxFinite = 0.0f;
        for (uint32_t i = 0; i < w * h; i++) {
            float d = data[i * 4 + 3];  // alpha channel
            if (d > maxFinite && d < 1e20f) maxFinite = d;
        }
        if (maxFinite <= 0.0f) maxFinite = 1.0f;

        std::vector<uint8_t> gray(static_cast<size_t>(w) * h, 0);
        for (uint32_t i = 0; i < w * h; i++) {
            float d = data[i * 4 + 3];
            float norm = std::max(0.0f, std::min(1.0f, d / maxFinite));
            gray[i] = static_cast<uint8_t>(norm * 255.0f);
        }
        stbi_write_png(path.c_str(), w, h, 1, gray.data(), w);
        std::cout << "Saved " << path << " (max hit-dist = " << maxFinite << " world units)" << std::endl;
    };
```

After the existing `--dump-specular` block, add two new blocks that reuse the existing `readbackDiffuseRadiance` / `readbackSpecularRadiance` helpers (they return the full RGBA32F vector; alpha is `data[i*4+3]`):

```cpp
    if (!dumpHitDistDiffusePath.empty()) {
        std::vector<float> data;
        uint32_t w = 0, h = 0;
        if (!renderer.readbackDiffuseRadiance(data, w, h)) {
            std::cerr << "[Hit-dist diffuse dump] readback failed\n";
        } else {
            dumpHitDistance(dumpHitDistDiffusePath, data, w, h);
        }
    }

    if (!dumpHitDistSpecularPath.empty()) {
        std::vector<float> data;
        uint32_t w = 0, h = 0;
        if (!renderer.readbackSpecularRadiance(data, w, h)) {
            std::cerr << "[Hit-dist specular dump] readback failed\n";
        } else {
            dumpHitDistance(dumpHitDistSpecularPath, data, w, h);
        }
    }
```

### Step 2.2: Build

```bash
cd /home/frankyin/Desktop/Github/ohao-3d-hitdist
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build.

### Step 2.3: Helmet scene full dump

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t2_3d_helmet.png 64 --denoise=none \
    --dump-diffuse=/tmp/t2_3d_diffuse.png \
    --dump-specular=/tmp/t2_3d_specular.png \
    --dump-hit-dist-diffuse=/tmp/t2_3d_hitdist_diffuse.png \
    --dump-hit-dist-specular=/tmp/t2_3d_hitdist_specular.png 2>&1 | tail -10
```

Expected stdout:
```
Saved /tmp/t2_3d_diffuse.png (max channel = X.X)
Saved /tmp/t2_3d_specular.png (max channel = Y.Y)
Saved /tmp/t2_3d_hitdist_diffuse.png (max hit-dist = A.A world units)
Saved /tmp/t2_3d_hitdist_specular.png (max hit-dist = B.B world units)
```

Record diff/spec radiance max channels AND max hit-distances.

### Step 2.4: Visual verification via Read tool

Read all 4 PNGs:

- **`/tmp/t2_3d_hitdist_specular.png`** (specular hit-distance): **visor + metal dome should be BRIGHT** (long virtual distance — env reflection is far / at infinity). Matte plates darker. Sky black.
- **`/tmp/t2_3d_hitdist_diffuse.png`** (diffuse hit-distance): matte plates show moderate gray (short single-segment distance to nearby geometry on bounce 1). Visor near-black (metal's diffuse is zero). Sky black.
- **`/tmp/t2_3d_diffuse.png`** (unchanged from 3.C.7 — RGB only).
- **`/tmp/t2_3d_specular.png`** (unchanged).

If the specular hit-dist dump is uniformly dark, the accumulation is broken. If diffuse is uniformly white or black, the latch isn't firing — investigate.

### Step 2.5: Copy to renders/ (gitignored)

```bash
mkdir -p renders
cp /tmp/t2_3d_hitdist_diffuse.png   renders/hit_dist_diffuse_helmet.png
cp /tmp/t2_3d_hitdist_specular.png  renders/hit_dist_specular_helmet.png
cp /tmp/t2_3d_helmet.png            renders/helmet_256spp_with_hitdist.png
```

### Step 2.6: Regression smoke — beauty unchanged

```bash
./build/cornell_box /tmp/t2_3d_cornell.png 64 --denoise=none 2>&1 | tail -3
```

Visually identical to pre-3.D.

### Step 2.7: Append verification_log.md entry

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. Append:

```markdown
## 2026-04-20: Hit-distance packing (Sub-plan 3.D)

Raygen packs hit-distance into alpha channel of bindings 22 (diffuse) and 23
(specular). NRD REBLUR consumes this for per-pixel spatial-filter sizing.

Semantics per NRD convention:
- Diffuse: single-segment. Distance from primary hit to first secondary hit
  (bounce 1's `payload.hitDist` in Stage C, captured via `diffHitRecorded` latch).
- Specular: accumulated along specular chain. Each Stage-B bounce adds
  `payload.hitDist` to `specHitDist` while `specChainActive` is true. Chain
  breaks on the first diffuse BSDF sample at bounce >=2 — flag flips false
  and accumulation stops. Matches NRD-canonical virtual-distance semantics
  for mirror-chain preservation.

Shader-only change. No new bindings, no C++ touches. Raw world-space
units; NRD-side normalization deferred to Sub-plan 4.

Verification on DamagedHelmet + env_studio, 64 spp, --denoise=none:
- **Specular hit-distance dump:** visor + metal dome bright (long virtual
  distance — env reflection effectively at infinity). Matte plates darker
  (single-segment reflection to nearby geometry). Sky black.
  Max: <fill from stdout> world units.
  Saved: `renders/hit_dist_specular_helmet.png`.
- **Diffuse hit-distance dump:** matte plates show moderate gray (short
  distance to bounce-1 diffuse GI source). Visor near-black (metals have
  zero diffuse albedo so the signal is suppressed downstream anyway).
  Max: <fill from stdout> world units.
  Saved: `renders/hit_dist_diffuse_helmet.png`.
- **Radiance dumps (unchanged from 3.C.7):** diffuse max <fill>; specular
  max <fill>.
- **Regression:** beauty output bit-identical to pre-3.D (alpha channel
  change has zero effect on the radiance sum).

Sub-plan 3.D complete. Next: Sub-plan 4 (NRD integration) — the jaw-drop
moment when realtime 1spp renders look like offline 1024spp.
```

Fill `<fill>` placeholders with actual measurements from Step 2.3.

### Step 2.8: Commit

```bash
git add examples/env_demo.cpp \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): --dump-hit-dist-* CLI + verification log (3.D)

Adds two env_demo flags that extract the alpha channel from the existing
readbackDiffuseRadiance / readbackSpecularRadiance RGBA32F readbacks and
encode as normalized grayscale PNG (max-finite normalization per dump).
No new readback helpers needed — alpha was already in the returned vector.

Helmet + env_studio verification:
- Specular hit-dist: visor/metal bright (long virtual distance on env
  reflection), matte plates darker.
- Diffuse hit-dist: matte plates moderate (short bounce-1 GI distance),
  visor near-black (metal).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §4.1 hit-distance locals | Task 1 Step 1.2 |
| §4.2 Stage B accumulation + chain break | Task 1 Steps 1.3, 1.4 |
| §4.3 Stage C single-segment capture | Task 1 Step 1.5 |
| §4.4 exit writes pack alpha | Task 1 Step 1.6 |
| §4.5 primary miss → both alphas 0 | Handled by existing zero-init + early-return path in Stage A (no code change needed; spec §4.5 notes this). |
| §4.6 all 3 raygens synced | Task 1 Steps 1.7 (offline), 1.8 (realtime) |
| §5.1 env_demo CLI + verification log | Task 2 |
| §6 helmet visual verification | Task 2 Steps 2.3, 2.4 |
| §10 success criteria 1-8 | Tasks 1 + 2 collectively |

**Placeholder scan:** only `<fill>` / `<fill from stdout>` in verification log template — intentional, filled at execution time.

**Type consistency:**
- `specHitDist` (float), `specChainActive` (bool), `diffHitDist` (float), `diffHitRecorded` (bool) — consistent across Task 1 steps + Task 2 verification descriptions.
- Alpha channel packing: `data[i*4+3]` — consistent with existing `readbackDiffuseRadiance`/`readbackSpecularRadiance` signature (returns `std::vector<float>` sized `width*height*4`).
- env_demo CLI prefix substring offsets: `--dump-hit-dist-diffuse=` = 24 chars (`substr(24)`); `--dump-hit-dist-specular=` = 25 chars (`substr(25)`). Verified.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-20-denoiser-subplan3d-hit-distance-packing.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Matches the pattern that shipped the whole 3.x chain cleanly.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
