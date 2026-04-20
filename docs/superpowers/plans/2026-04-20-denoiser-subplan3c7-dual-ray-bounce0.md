# Denoiser Sub-plan 3.C.7: Dual-Ray Bounce-0 Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** At the primary hit, trace two independent indirect rays (diffuse-sampled + specular-sampled) instead of one stochastic sample. Eliminates the bounce-0 `lobeMask` coin-flip. Both radiance AOVs receive light every frame; NRD input variance drops dramatically at 1 spp.

**Architecture:** Shader-only sub-plan — no C++ touches. All three raygens restructure main() into three stages: (A) primary ray + first-hit capture + AOVs + analytic direct NEE/env-MIS split, (B) specular indirect inner loop, (C) diffuse indirect inner loop. Beauty = `diffContrib + specContrib`. Inline duplication of Stage B/C inner loops (helper-function extraction parked for Sub-plan 4). Per-path MIS state with `spec*` / `diff*` prefixes; `lobeMask` deleted entirely.

**Tech Stack:** Vulkan 1.3 · GLSL ray tracing · no new bindings · no C++ side · no new libs.

**Reference spec:** `docs/superpowers/specs/2026-04-20-denoiser-subplan3c7-dual-ray-bounce0-design.md`

---

## File Structure

**New files:** none.

**Modified files:**

| Path | Change |
|------|--------|
| `shaders/rt/pt_raygen.rgen` | Main restructure: Stage A (primary ray + AOVs + analytic direct) + Stage B (spec indirect loop 1..maxBounces) + Stage C (diff indirect loop 1..maxBounces). Delete `lobeMask` + all bounce-0 stamp sites + bounce-≥1 attribution branches. Per-path MIS state (`spec*`, `diff*`). Sampler dim partitioning. |
| `shaders/rt/pt_raygen_offline.rgen` | Verbatim mirror of pt_raygen.rgen. |
| `shaders/rt/pt_raygen_realtime.rgen` | Same restructure, with no env-MIS block inside Stage B/C loops (realtime has none). |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.C.7 entry with before/after 1spp + 256spp observations. |

**Scope is shader-only.** No C++ changes. No new descriptor bindings. Pool, barriers, AOV layout all unchanged from 3.C.6.

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-3c7-dualray -b denoiser-3c7-dual-ray-bounce0 HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-3c7-dualray`.

If the fresh worktree's `build/` is empty, configure + bootstrap from master if needed:
```bash
cd /home/frankyin/Desktop/Github/ohao-3c7-dualray
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON 2>&1 | tail -10
# If build/_deps/glm-src/ is empty:
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/. build/_deps/
```

For OptiX (optional):
```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: Restructure pt_raygen.rgen to dual-ray

**Files:**
- Modify: `shaders/rt/pt_raygen.rgen`

This is the reference shader. Task 2 mirrors it to the offline variant. Task 3 adapts to realtime (shorter — no env-MIS).

### Step 1.1: Read current pt_raygen.rgen

Before editing, the implementer must read `shaders/rt/pt_raygen.rgen` top-to-bottom. Key landmarks in the current post-3.C.6 state:

- **Lines ~34-37:** binding 24/25 decls (rgba8) — KEEP unchanged.
- **Lines ~111-125:** local accumulator block. Currently: `radiance`, `throughput`, `firstHitPos`, `firstHitDist`, `lastBsdfPdf`, `lastBounceWasDelta`, `diffContrib`, `specContrib`, `firstHitDiffAlbedo`, `firstHitSpecColor`, `lobeMask`.
- **Lines ~127-135:** zero-init imageStore for diff-albedo/spec-color AOVs — KEEP unchanged.
- **Lines ~136-208:** the single-ray `for (uint bounce = 0u; bounce <= maxBounces; bounce++) { ... }` loop. This ENTIRELY restructures.
- **Lines ~538-619:** post-loop accumulation + tonemap + output imageStore + raw radiance AOV writes — KEEP mostly unchanged (just read from final `diffContrib`/`specContrib`/`radiance` whose values are now computed by Stage A + B + C).

### Step 1.2: Write the target restructured main() body

The new main() shape (pseudocode-close to target GLSL; use current code's exact math/vars):

```glsl
void main() {
    ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
    uint frameIdx = pc.params.z;
    uint historyFrameCount = pc.control.y;
    uint maxBounces = pc.params.w;
    bool enableAOVs = (pc.control.x & PT_FLAG_ENABLE_AOVS) != 0u;
    bool enableInternalDenoise = (pc.control.x & PT_FLAG_ENABLE_INTERNAL_DENOISE) != 0u;
    bool enableFireflyClamp = (pc.control.x & PT_FLAG_ENABLE_FIREFLY_CLAMP) != 0u;

    samplerInit(uvec2(pixel), frameIdx);
    uint dimIdx = 0u;

    // Camera ray (unchanged from current)
    vec2 jitter = getSample2D(dimIdx); dimIdx += 2u;
    vec2 uv = (vec2(pixel) + 0.5 + jitter) / vec2(pc.params.xy);
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 camPos = vec3(pc.invView[3]);
    vec3 fwd = -vec3(pc.invView[2]);
    vec3 right = vec3(pc.invView[0]);
    vec3 up = vec3(pc.invView[1]);
    float aspect = float(pc.params.x) / float(pc.params.y);
    float tanFovY = abs(pc.invProj[1][1]);
    float tanFovX = tanFovY * aspect;
    vec3 rayDir = normalize(fwd + right * ndc.x * tanFovX - up * ndc.y * tanFovY);
    vec3 rayOrigin = camPos;

    // Accumulators (split; no more lobeMask)
    vec3 radiance           = vec3(0.0);
    vec3 diffContrib        = vec3(0.0);
    vec3 specContrib        = vec3(0.0);
    vec3 firstHitPos        = vec3(0.0);
    float firstHitDist      = -1.0;
    vec3 firstHitDiffAlbedo = vec3(0.0);
    vec3 firstHitSpecColor  = vec3(0.04);

    // Zero-init demod AOVs (miss writes these unless Stage A overwrites) — unchanged from 3.C.6.
    imageStore(diffuseAlbedoAOV, pixel, vec4(0.0));
    imageStore(specColorAOV,     pixel, vec4(0.0));

    // ================================================================
    // STAGE A: Primary ray + first-hit capture + AOVs + analytic direct
    // ================================================================
    payload.hitDist = -1.0;
    traceRayEXT(topLevelAS, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0,
                 rayOrigin, 0.001, rayDir, 10000.0, 0);
    firstHitPos  = payload.hitPos;
    firstHitDist = payload.hitDist;

    // [Sub-plan 3.A] motion vector AOV — move the existing bounce==0 block here.
    //   Uses firstHitPos, historyFrameCount, pc.prevViewProj, pc.params, pc.invView, pc.invProj.
    //   Writes imageStore(motionVector, pixel, vec4(motion, 0, 0)).

    // [Sub-plan 3.B] depth + roughness AOVs — existing bounce==0 block here.
    //   Uses firstHitPos, pc.invView, payload.attenuation.x.
    //   Writes imageStore(depthAOV, ...) + imageStore(roughnessAOV, ...).

    if (firstHitDist < 0.0) {
        // Primary miss: beauty = env radiance (payload.color from miss shader).
        // Both diff/spec channels stay zero (zero-init + no Stage B/C run).
        radiance = payload.color;  // throughput is 1 at bounce 0 so throughput*color = color
        if (enableAOVs) {
            imageStore(albedoAOV, pixel, vec4(payload.color, 1.0));
            imageStore(normalAOV, pixel, vec4(0));
        }
        // Skip Stage B + Stage C; jump to composite.
    } else {
        vec3 hitPos   = payload.hitPos;
        vec3 N        = payload.hitNormal;
        vec3 albedo   = payload.hitAlbedo;
        vec3 emissive = payload.color;

        // First-hit AOVs (existing enableAOVs gate).
        if (enableAOVs) {
            imageStore(albedoAOV, pixel, vec4(albedo, 1.0));
            imageStore(normalAOV, pixel, vec4(N * 0.5 + 0.5, 1.0));
        }

        // Material decode (existing code).
        float packedRoughness = payload.attenuation.x;
        bool  isMetal = (packedRoughness < 0.0);
        float roughness = abs(packedRoughness);
        if (roughness >= 10.0) roughness -= 10.0;
        roughness = max(roughness, 0.01);
        vec3 F0 = isMetal ? albedo : vec3(0.04);

        firstHitDiffAlbedo = isMetal ? vec3(0.0) : albedo;
        firstHitSpecColor  = F0;
        imageStore(diffuseAlbedoAOV, pixel, vec4(firstHitDiffAlbedo, 1.0));
        imageStore(specColorAOV,     pixel, vec4(firstHitSpecColor,  1.0));

        // Emissive at primary hit → diffuse channel (3.C.6 convention).
        if (length(emissive) > 0.001) {
            vec3 emissiveContribution = emissive;  // throughput == 1 here
            radiance += emissiveContribution;
            diffContrib += emissiveContribution;
        }

        // ==== Analytic direct NEE split at bounce 0 (3.C code lifted verbatim) ====
        // Full NEE block from current pt_raygen.rgen lines ~220-355, WITH the 3.C
        // split logic inside. Since we're at bounce 0, the `if (bounce == 0u)` branch
        // always fires (analytic split via existing `diff`/`spec` BRDF terms).
        // The `else if (lobeMask == 0u) ...` branches GO AWAY — only bounce-0 path fires here.
        // (See Step 1.3 for the exact block — copy-paste and simplify.)

        // ==== Analytic direct env-MIS split at bounce 0 (3.C code lifted) ====
        // Same as above: full env-MIS block from current lines ~358-493, with only
        // the bounce-0 analytic split branch retained. lobeMask attribution GONE.

        // ================================================================
        // STAGE B: Specular-sampled indirect path (bounces 1..maxBounces)
        // ================================================================
        // Sample the specular lobe from first hit. Uses the SAME math as the current
        // "specular branch" in the BSDF-sample block at lines ~513-531.
        vec3 specDir;
        vec3 specThroughput;
        float specLastBsdfPdf;
        bool  specLastBounceWasDelta;
        {
            float cosI = abs(dot(normalize(rayDir), N));
            vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - cosI, 5.0);
            float specProb = max(fresnel.r, max(fresnel.g, fresnel.b)) * (1.0 - roughness * 0.9);
            if (isMetal) specProb = 1.0;

            vec3 inDir = rayDir;
            vec3 reflected = reflect(rayDir, N);
            if (roughness > 0.01) {
                vec2 jitU = getSample2D(dimIdx); dimIdx += 2u;
                vec3 jitVec = cosineHemisphere(reflected, jitU) * roughness;
                reflected = normalize(reflected + jitVec);
                if (dot(reflected, N) < 0.0) {
                    vec2 fallbackU = getSample2D(dimIdx); dimIdx += 2u;
                    reflected = cosineHemisphere(N, fallbackU);
                }
            }
            specDir = reflected;
            specThroughput = isMetal ? albedo : vec3(1.0);
            specThroughput /= max(specProb, 0.01);

            if (roughness < 0.05) {
                specLastBsdfPdf = 1.0;
                specLastBounceWasDelta = true;
            } else {
                vec3 Hspec = normalize(-inDir + specDir);
                float NdotH_s = max(dot(N, Hspec), 0.001);
                float VdotH_s = max(dot(-inDir, Hspec), 0.001);
                float as = roughness * roughness;
                float as2 = as * as;
                float denomGGX = NdotH_s * NdotH_s * (as2 - 1.0) + 1.0;
                float D_s = as2 / (OHAO_PI * denomGGX * denomGGX + 1e-4);
                float pdf_spec = D_s * NdotH_s / (4.0 * VdotH_s + 1e-4);
                specLastBsdfPdf = specProb * pdf_spec;
                specLastBounceWasDelta = false;
            }
        }

        vec3 specRayOrigin = hitPos + N * 0.01;
        vec3 specRayDir = specDir;

        for (uint bounce = 1u; bounce <= maxBounces; bounce++) {
            payload.hitDist = -1.0;
            traceRayEXT(topLevelAS, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0,
                         specRayOrigin, 0.001, specRayDir, 10000.0, 0);

            if (payload.hitDist < 0.0) {
                // Env miss with MIS weighting (same formula as current loop)
                float envMisWeight = 1.0;
                if (payload.envPdf > 0.0 && pc.control.w > 0u && !specLastBounceWasDelta) {
                    envMisWeight = misBalanceHeuristic(specLastBsdfPdf, payload.envPdf);
                }
                vec3 envMissContribution = specThroughput * payload.color * envMisWeight;
                radiance    += envMissContribution;
                specContrib += envMissContribution;
                break;
            }

            vec3 bHitPos = payload.hitPos;
            vec3 bN = payload.hitNormal;
            vec3 bAlbedo = payload.hitAlbedo;
            vec3 bEmissive = payload.color;

            if (length(bEmissive) > 0.001) {
                vec3 ec = specThroughput * bEmissive;
                radiance    += ec;
                specContrib += ec;
            }

            float bPackedRoughness = payload.attenuation.x;
            bool  bIsMetal = (bPackedRoughness < 0.0);
            float bRoughness = abs(bPackedRoughness);
            if (bRoughness >= 10.0) bRoughness -= 10.0;
            bRoughness = max(bRoughness, 0.01);
            vec3 bF0 = bIsMetal ? bAlbedo : vec3(0.04);

            // NEE (full block from current pt_raygen.rgen; contribution goes entirely
            // to specContrib; NO per-bounce split logic, NO lobeMask attribution.)
            // Paste the current NEE block (lines ~220-355) but with:
            //   - hitPos → bHitPos, N → bN, albedo → bAlbedo, F0 → bF0, roughness → bRoughness,
            //     isMetal → bIsMetal, rayDir → specRayDir, throughput → specThroughput
            //   - REMOVE the `if (bounce == 0u) { diff/spec split } else if ...` structure
            //   - Replace with a single `specContrib += directContribution;` (and `radiance += ...`).

            // Env-MIS (full block, same substitutions; single accumulator target = specContrib).

            // Russian roulette (unchanged form):
            if (bounce > 1u) {
                float p = max(specThroughput.r, max(specThroughput.g, specThroughput.b));
                float rrRand = getSample1D(dimIdx); dimIdx += 1u;
                if (p < 0.01 || rrRand > p) break;
                specThroughput /= p;
            }

            // BSDF sample for next bounce (full block from current lines ~504-531, but
            // write to specRayDir/specRayOrigin/specThroughput and update
            // specLastBsdfPdf/specLastBounceWasDelta; DELETE `if (bounce == 0u) lobeMask = X;`).
            // This updates state for the NEXT iteration of this specular loop.
        }

        // ================================================================
        // STAGE C: Diffuse-sampled indirect path (bounces 1..maxBounces)
        // ================================================================
        // Same shape as Stage B but:
        //   - Initial sample uses the cosine-weighted diffuse lobe.
        //   - All `spec*` variables renamed to `diff*`.
        //   - Accumulator target = diffContrib.
        vec3 diffDir;
        vec3 diffThroughput;
        float diffLastBsdfPdf;
        bool  diffLastBounceWasDelta;
        {
            float cosI = abs(dot(normalize(rayDir), N));
            vec3 fresnel = F0 + (1.0 - F0) * pow(1.0 - cosI, 5.0);
            float specProb = max(fresnel.r, max(fresnel.g, fresnel.b)) * (1.0 - roughness * 0.9);
            if (isMetal) specProb = 1.0;

            vec2 diffU = getSample2D(dimIdx); dimIdx += 2u;
            diffDir = cosineHemisphere(N, diffU);
            diffThroughput = albedo / max(1.0 - specProb, 0.01);
            diffLastBsdfPdf = (1.0 - specProb) * max(dot(diffDir, N), 0.0) / OHAO_PI;
            diffLastBounceWasDelta = false;
        }

        vec3 diffRayOrigin = hitPos + N * 0.01;
        vec3 diffRayDir = diffDir;

        for (uint bounce = 1u; bounce <= maxBounces; bounce++) {
            // Identical structure to Stage B spec loop, with diff* locals + diffContrib.
            // [Full copy of Stage B inner loop with s/spec/diff/g substitution.]
        }
    }  // end of "firstHitDist >= 0.0" else branch

    // ================================================================
    // Beauty composite
    // ================================================================
    radiance = diffContrib + specContrib;
    // Note: for primary miss the radiance was set to env color above, and
    // diff/specContrib are zero — radiance becomes zero here. Fix:
    // move the miss-branch `radiance = payload.color` AFTER this sum, OR
    // only compute `radiance = diffContrib + specContrib` when firstHitDist > 0.0.

    if (enableFireflyClamp && pc.tuning.x > 0.0) {
        float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
        if (lum > pc.tuning.x) radiance *= pc.tuning.x / lum;
    }

    // Accumulation + reprojection block (unchanged from current lines ~538-560).
    // Internal denoise block (unchanged).
    // ACES tonemap + imageStore(outputImage, ...) (unchanged).

    // Raw radiance AOV writes (3.C.6 semantics, unchanged):
    imageStore(diffuseRadiance,  pixel, vec4(diffContrib, 1.0));
    imageStore(specularRadiance, pixel, vec4(specContrib, 1.0));
}
```

### Step 1.3: Apply the restructure

This is a large atomic edit. The implementer will:

1. **Read the current raygen's NEE block and env-MIS block** thoroughly — they get pasted into Stages B and C with substitutions.
2. **Replace the entire `for (uint bounce = 0u; bounce <= maxBounces; bounce++)` loop** (and all code inside it) with the Stage A / Stage B / Stage C structure shown above.
3. **Keep the pre-loop local declarations** but DELETE `uint lobeMask = 0u;` and DELETE the old `lastBsdfPdf`/`lastBounceWasDelta` (they become per-path state in Stages B and C).
4. **Keep the zero-init imageStore of diffuse/spec-color AOVs** at the top.
5. **Keep the post-loop accumulation/tonemap/output block** intact.
6. **Move the primary-miss handling** to inside Stage A so the "radiance = payload.color" works correctly. The `radiance = diffContrib + specContrib` composite only applies on hit.

**Important substitution rules for the inner loop duplicate (Stage B → Stage C):**
- `specRayDir` ↔ `diffRayDir`
- `specRayOrigin` ↔ `diffRayOrigin`
- `specThroughput` ↔ `diffThroughput`
- `specLastBsdfPdf` ↔ `diffLastBsdfPdf`
- `specLastBounceWasDelta` ↔ `diffLastBounceWasDelta`
- `specContrib` ↔ `diffContrib`
- Inside the BSDF-sample block at the end of each inner iteration: the sample updates the SAME-prefixed variables for the next iteration.

### Step 1.4: Verify primary-miss handling

Pre-3.C.7, primary miss writes `radiance = throughput * payload.color * envMisWeight;` and breaks. Since `throughput=1` and `envMisWeight=1` on the primary ray (bounce==0, `lastBounceWasDelta=true`), the net effect is `radiance = payload.color`.

Post-3.C.7, same semantics, written inside the miss branch of Stage A. Be sure the `radiance = diffContrib + specContrib` composite at the end of main() doesn't zero this out. Suggested structure:

```glsl
if (firstHitDist < 0.0) {
    // ... miss branch: write AOVs, skip Stage B + C
    // radiance is set here
} else {
    // ... Stage A direct + Stage B + Stage C
    radiance = diffContrib + specContrib;
}
// Accumulation + tonemap + output (always runs)
```

### Step 1.5: Build shaders + full app

```bash
cd /home/frankyin/Desktop/Github/ohao-3c7-dualray
cmake --build build --target shaders -j8 2>&1 | tail -10
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean shader compile, clean link. If GLSL fails to compile, fix syntax and retry.

### Step 1.6: Cornell smoke — runs without crash

```bash
./build/cornell_box /tmp/t1_3c7_cornell.png 16 --denoise=none 2>&1 | tail -3
```

Expected: `Saved: /tmp/t1_3c7_cornell.png`. No validation errors. NOTE: pt_raygen.rgen may or may not be the active raygen for cornell_box — if cornell_box uses RTOffline with pt_raygen_offline.rgen, this smoke confirms the build is healthy but does not yet exercise the new code. Tasks 2+3 mirror to the active profile raygens.

### Step 1.7: Commit

```bash
git add shaders/rt/pt_raygen.rgen
git commit -m "feat(rt): dual-ray bounce-0 split in pt_raygen.rgen (Sub-plan 3.C.7)

Restructures main() into three stages:
- Stage A: primary ray + first-hit capture + AOVs (MV, depth, roughness,
  diff-albedo, spec-color, normal, albedo) + analytic direct NEE/env-MIS
  split at bounce 0.
- Stage B: specular-sampled indirect path (bounces 1..maxBounces).
  Standalone inner loop, accumulates into specContrib.
- Stage C: diffuse-sampled indirect path (bounces 1..maxBounces).
  Standalone inner loop, accumulates into diffContrib.

Beauty = diffContrib + specContrib (mean matches pre-3.C.7; per-sample
variance drops because each pixel samples both lobes every frame
instead of stochastically picking one).

Removed: uint lobeMask local + bounce-0 stamp sites + bounce-≥1
attribution branches in NEE/env-MIS/emissive/env-miss blocks. MIS
state split into per-path specLastBsdfPdf/Delta + diffLastBsdfPdf/Delta.

Offline + realtime raygens updated in subsequent commits.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Task 2: Mirror to pt_raygen_offline.rgen

**Files:**
- Modify: `shaders/rt/pt_raygen_offline.rgen`

### Step 2.1: Verbatim copy of main() from pt_raygen.rgen

`pt_raygen_offline.rgen` is a verbatim mirror of `pt_raygen.rgen` except for the top 3-4 line comment block. After Task 1, the offline file is out of sync.

Overwrite the offline file's main() body (everything from `void main() {` to its closing `}`) with the restructured version from pt_raygen.rgen. Keep the top comment block.

### Step 2.2: Verify parity

```bash
cd /home/frankyin/Desktop/Github/ohao-3c7-dualray
diff shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen | head
```

Expected: only the top comment block differs (lines 5-8 vs lines 5-6 or similar).

### Step 2.3: Build + smoke

```bash
cmake --build build --target shaders -j8 2>&1 | tail -5
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t2_3c7_cornell.png 16 --denoise=none 2>&1 | tail -3
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t2_3c7_helmet.png 16 --denoise=none 2>&1 | tail -3
```

After this commit, the offline profile uses dual-ray. Cornell + env_demo smoke now exercises the new code path. Expected: clean renders, no validation errors. Beauty at 16spp should be visually close to pre-3.C.7 (statistical equivalence; some per-pixel noise difference expected).

### Step 2.4: Commit

```bash
git add shaders/rt/pt_raygen_offline.rgen
git commit -m "feat(rt): mirror dual-ray restructure to pt_raygen_offline.rgen

Verbatim copy of pt_raygen.rgen's main() body (differs only in top
comment header). Offline profile now uses dual-ray at bounce 0.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Apply dual-ray to pt_raygen_realtime.rgen

**Files:**
- Modify: `shaders/rt/pt_raygen_realtime.rgen`

### Step 3.1: Read current realtime raygen

Realtime has a shorter structure than the default/offline raygen:
- NO env-MIS block (only NEE + emissive + env-miss + BSDF sample)
- Shorter accumulation/temporal reprojection block (uses `prevSurfaceHistory` / `prevShadingHistory`)

All other restructure logic carries over — Stage A (primary + AOVs + analytic NEE split), Stage B (spec indirect), Stage C (diff indirect).

### Step 3.2: Apply restructure, adapting for realtime

Apply the same Stage A + B + C structure, but INSIDE Stage B and Stage C inner loops:

- **Keep** the NEE block (same as default, goes fully to `specContrib` / `diffContrib`)
- **SKIP** the env-MIS block (realtime doesn't have one)
- **Keep** emissive, env-miss, Russian roulette, BSDF sample

Stage A's analytic NEE split is unchanged (realtime has the same analytic NEE at bounce 0). Stage A has no env-MIS split to perform.

### Step 3.3: Verify no env-MIS leaks into realtime

After edit, grep to confirm no env-MIS references accidentally got pasted from default:

```bash
grep -n "envMap\|envRadiance\|sampleEnvMap\|envMisWeight" shaders/rt/pt_raygen_realtime.rgen
```

Expected: only references are in contexts that already exist in realtime (e.g., `envMisWeight` in the env-miss handling at end of Stage B/C inner loops is OK because realtime does use env as background). If new env-MIS-sampling block leaked in, remove it.

### Step 3.4: Build + smoke

```bash
cmake --build build --target shaders -j8 2>&1 | tail -5
cmake --build build -j8 2>&1 | tail -5
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t3_3c7_helmet.png 16 rt_realtime --denoise=none 2>&1 | tail -3
```

Expected: realtime renders clean. If env_demo doesn't support `rt_realtime` arg at this position, substitute the appropriate CLI flag (check existing argv handling in env_demo.cpp). Smoke only — beauty quality verification deferred to T4.

### Step 3.5: Commit

```bash
git add shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): dual-ray restructure for pt_raygen_realtime.rgen

Same Stage A + B + C structure as default/offline raygens, adapted
for realtime: Stage B + C inner loops skip env-MIS block (realtime
has none). NEE, emissive, env-miss, Russian roulette, BSDF sample
all identical to default.

Realtime profile now uses dual-ray. Cost: 1 primary + 2×maxBounces
rays. For realtime maxBounces=1, that's 2→3 rays (~1.5x).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Visual verification + verification log

**Files:**
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

### Step 4.1: 1 spp visual comparison (the key test)

The hallmark of 3.C.7 is the 1 spp visual improvement. Dump helmet at 1 spp with dual-ray active (current worktree state), save, compare mentally against 3.C.6 expectations (one channel zero, other channel full, per-pixel stochastic).

```bash
cd /home/frankyin/Desktop/Github/ohao-3c7-dualray
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t4_3c7_helmet_1spp.png 1 --denoise=none \
    --dump-diffuse=/tmp/t4_3c7_diffuse_1spp.png \
    --dump-specular=/tmp/t4_3c7_specular_1spp.png \
    --dump-diff-albedo=/tmp/t4_3c7_diff_albedo_1spp.png \
    --dump-spec-color=/tmp/t4_3c7_spec_color_1spp.png 2>&1 | tail -8
```

Record max channel values for diffuse + specular.

Expected observations (via Read tool on the PNGs):
- **`diffuse_1spp.png`:** matte regions show ambient lighting on EVERY PIXEL (not stochastic speckle where half the pixels are zero). Emissive panels bright.
- **`specular_1spp.png`:** visor shows env reflection on EVERY PIXEL (not stochastic speckle). Metal body shows reflection on every pixel.
- If either AOV shows a "polka dot" pattern (every other pixel zero), dual-ray is NOT working — investigate.

### Step 4.2: 256 spp convergence regression

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/t4_3c7_helmet_256spp.png 256 --denoise=none 2>&1 | tail -3
```

Read the beauty PNG. Should look visually equivalent to any prior high-spp helmet render (memory of 3.C.6 / 3.C.5 results applies). If beauty looks dramatically different (e.g. flat-shaded, wrong colors), there's a bug — investigate.

### Step 4.3: Cornell regression

```bash
./build/cornell_box /tmp/t4_3c7_cornell_64spp.png 64 --denoise=none 2>&1 | tail -3
```

Standard Cornell box visual should hold: red + green walls, white diffuse spheres, glossy reflections. Any visible corruption → bug.

### Step 4.4: Copy PNGs to renders/

```bash
mkdir -p renders
cp /tmp/t4_3c7_helmet_1spp.png      renders/helmet_1spp_dualray.png
cp /tmp/t4_3c7_diffuse_1spp.png     renders/diffuse_1spp_dualray.png
cp /tmp/t4_3c7_specular_1spp.png    renders/specular_1spp_dualray.png
cp /tmp/t4_3c7_helmet_256spp.png    renders/helmet_256spp_dualray.png
```

renders/ is gitignored — do NOT `git add` these PNGs.

### Step 4.5: Append verification_log.md entry

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. Append:

```markdown
## 2026-04-20: Dual-ray bounce-0 split (Sub-plan 3.C.7)

At primary hit, raygen now traces two independent indirect paths (one
diff-sampled, one spec-sampled) instead of stochastically picking one
lobe. `lobeMask` deleted entirely. Each path has its own MIS state.

Cost: 1 primary + 2×maxBounces indirect rays (~1.8× for maxBounces=4;
~1.5× for realtime maxBounces=1). Shader-only change — no new bindings.

Verification on DamagedHelmet + env_studio:

- **1 spp, --denoise=none:** diffuse AOV shows ambient lighting on
  every matte pixel; specular AOV shows env reflection on every
  glossy pixel. No more "polka dot" stochastic lobe pattern.
  diffuse max: <fill from stdout>. specular max: <fill from stdout>.
  Compared to pre-3.C.7's 1spp render where half the pixels were
  zero in one channel or the other — dramatically cleaner.
- **256 spp, --denoise=none:** beauty converges to the same image
  as pre-3.C.7 (both are unbiased Monte Carlo). Visual mean within
  standard noise.
- **Cornell 64spp regression:** standard Cornell box (red/green
  walls, spheres) renders correctly.

Visible win at low spp: **yes** — first sub-plan in the 3.x chain
to produce a user-visible quality improvement. At high spp
convergence unchanged.

Follow-ups for Sub-plan 4:
- If realtime frame time tanks with dual-ray, add a specialization
  constant to toggle back to single-ray per profile.
- Extract Stage B/C inner loop into `indirect_path.glsl` helper
  to eliminate the ~130-line duplication (bundled with readback
  helper extraction).

Sub-plan 3.C.7 complete. Next: 3.D (ping-pong depth for NRD
disocclusion).
```

Fill `<fill from stdout>` with actual max channel values from Step 4.1.

### Step 4.6: Commit

```bash
git add tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): 3.C.7 verification log — dual-ray bounce-0 ships visible 1spp win

Records 1spp helmet comparison (diff/spec AOVs filled on every
pixel, no polka-dot stochastic lobe pattern) + 256spp convergence
match vs pre-3.C.7 + Cornell regression pass.

First visible quality improvement in the 3.x chain. At high spp,
results converge identically to pre-3.C.7 (both unbiased MC).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §4.1 raygen restructure (Stage A + B + C) | Task 1 (+ mirrored by 2, 3) |
| §4.2 remove lobeMask + stamps + attribution branches | Task 1 |
| §4.3 inline-duplicated Stage B/C loops | Task 1 |
| §4.4 Stage A first-hit AOVs preserved | Task 1 |
| §4.5 primary-miss path handling | Task 1 Step 1.4 |
| §4.6 NEE/env-MIS at bounce-0 = analytic split; bounce-≥1 = per-path | Task 1 |
| §4.7 3-raygen sync (default, offline, realtime) | Tasks 1, 2, 3 |
| §4.8 sampler dim partitioning | Implicit in Stage B/C dim-consumption ordering (Stage B before Stage C in main()) |
| §6 verification — 1spp visual win + 256spp regression | Task 4 |
| §9 success criteria #1-10 | Tasks 1-4 collectively |

**Placeholder scan:** only `<fill from stdout>` in the verification log template — intentional, filled at execution time.

**Type consistency:**
- Per-path locals: `specRayDir`/`diffRayDir`, `specRayOrigin`/`diffRayOrigin`, `specThroughput`/`diffThroughput`, `specLastBsdfPdf`/`diffLastBsdfPdf`, `specLastBounceWasDelta`/`diffLastBounceWasDelta`, `specContrib`/`diffContrib`. Consistent across Tasks 1, 2, 3.
- Stage names used consistently: A (primary + AOVs + direct), B (spec indirect), C (diff indirect).
- `firstHitDiffAlbedo`/`firstHitSpecColor` (from 3.C.6) retained — consumed by Stage A AOV writes only.
- No new GLSL bindings introduced — 24/25 unchanged from 3.C.6.

**Known ambiguity handled inline:**
- Step 1.3 explicitly lists variable substitution rules for Stage C duplicate.
- Step 1.4 explicitly documents primary-miss handling.
- Step 3.2 explicitly calls out the realtime env-MIS SKIP.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-20-denoiser-subplan3c7-dual-ray-bounce0.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Pattern that shipped 3.A + 3.B + 3.C + 3.C.5 + 3.C.6 cleanly.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
