# Denoiser Pipeline Sub-plan 3.D — Hit-Distance Packing — Design

**Date:** 2026-04-20
**Status:** Approved design, pending implementation plan
**Phase:** Denoiser Pipeline prep for Sub-plan 4 (NRD integration)
**Predecessors:** 3.A ✅, 3.B ✅, 3.C ✅, 3.C.5 ✅, 3.C.6 ✅, 3.C.7 ✅

---

## 1. Goal

Pack per-pixel hit-distance into the alpha channel of bindings 22 (diffuse radiance) and 23 (specular radiance). REBLUR (the NRD denoiser variant matching our diffuse/specular split) uses this to size its spatial filter per pixel — without it, reflections blur or noise.

Scope reframe: the original "3.D = ping-pong prev-depth" premise was incorrect. NRD REBLUR maintains its own internal depth history and doesn't require a prev-depth input. What it *does* require is `IN_DIFF_RADIANCE_HITDIST` and `IN_SPEC_RADIANCE_HITDIST` — packed RGBA where RGB = demodulated radiance and A = hit-distance. That's what 3.D now addresses.

## 2. Non-Goals

- Ping-pong prev-depth (out of scope; NRD doesn't need it).
- NRD integration / the normalization pass (`REBLUR_FrontEnd_PackRadianceAndNormHitDist`) lives in Sub-plan 4.
- Pass split refactor (Sub-plan 3.E if still needed).
- Skeletal motion vectors (3.F).

## 3. Decisions (from brainstorming)

- **Diffuse hit-distance: single-segment.** Distance from primary hit to first diffuse-sampled indirect hit (bounce 1's `payload.hitDist` in Stage C). NRD's diffuse channel is low-frequency; single-segment is sufficient and matches reference integrations.
- **Specular hit-distance: accumulated through specular chain, strict break on diffuse sample.** Starts `specChainActive = true`, accumulates each bounce's `payload.hitDist` while flag is true. On first diffuse BSDF sample at bounce ≥2 inside Stage B's inner loop, flag flips false — accumulation stops. Path subsequently terminating via miss or RR keeps the accumulated value. Matches NRD-canonical "virtual distance" semantics for sharp-reflection preservation.
- **Raw world-space units.** Normalization happens at the NRD boundary in Sub-plan 4 (via `REBLUR_FrontEnd_PackRadianceAndNormHitDist`). Our shader writes unnormalized distance — simpler, scene-invariant, no magic normalization constants embedded.
- **Miss / degenerate cases: write 0.** If bounce 1 misses immediately (no first-secondary-hit), hit-distance = 0. NRD interprets 0 as "no valid sample" per its docs. Primary miss (Stage A early-out) also writes 0 to both channels.
- **Shader-only sub-plan.** No C++ changes. No new descriptor bindings. Alpha channel of existing RGBA32F at 22/23 is already float-precision.

## 4. Architecture

### 4.1 New locals in raygen main()

After the existing Stage A/B/C accumulator block, before Stage A begins, declare:

```glsl
// Sub-plan 3.D: hit-distance per NRD channel convention
float specHitDist = 0.0;          // accumulated along specular chain
bool  specChainActive = true;     // true until a diffuse BSDF sample breaks the chain
float diffHitDist = 0.0;          // single-segment: first secondary hit
bool  diffHitRecorded = false;    // set once on first Stage-C bounce-1 hit
```

### 4.2 Stage B inner loop — specular chain accumulation

Immediately after `traceRayEXT(...)` at the top of each Stage B inner-loop iteration, BEFORE the miss branch:

```glsl
if (payload.hitDist >= 0.0 && specChainActive) {
    specHitDist += payload.hitDist;
}
```

Inside Stage B's bounce-≥2 BSDF-sample block, in the diffuse-sample branch (the `else` that's taken when `bsdfChoice >= specProb`):

```glsl
specChainActive = false;  // specular chain broken; stop accumulating further hit-distance
```

### 4.3 Stage C inner loop — single-segment diffuse capture

Immediately after `traceRayEXT(...)` at the top of each Stage C inner-loop iteration, BEFORE the miss branch:

```glsl
if (!diffHitRecorded && payload.hitDist >= 0.0) {
    diffHitDist = payload.hitDist;
    diffHitRecorded = true;
}
```

The flag ensures we capture only bounce 1's hit; subsequent bounces inside Stage C's indirect path don't overwrite.

### 4.4 Exit writes

Replace the current 3.C.6 writes:

```glsl
imageStore(diffuseRadiance,  pixel, vec4(diffContrib, 1.0));
imageStore(specularRadiance, pixel, vec4(specContrib, 1.0));
```

With:

```glsl
// Sub-plan 3.D: alpha channel carries hit-distance for NRD REBLUR
imageStore(diffuseRadiance,  pixel, vec4(diffContrib, diffHitDist));
imageStore(specularRadiance, pixel, vec4(specContrib, specHitDist));
```

### 4.5 Primary miss branch

In the Stage A primary-miss branch, the existing zero-init of the diff/spec AOVs already handles this case — both alphas will be 0, which is the NRD "no sample" convention. No additional code needed.

### 4.6 All 3 raygens

The hit-distance logic is structurally identical across `pt_raygen.rgen`, `pt_raygen_offline.rgen`, and `pt_raygen_realtime.rgen`. No env-MIS-related divergence affects this sub-plan. Single atomic edit across all 3 files (Task 1).

## 5. Integration Points

### 5.1 File map

| Path | Change |
|------|--------|
| `shaders/rt/pt_raygen.rgen` | Hit-dist locals + flag; Stage B accumulation + chain-break; Stage C single-segment capture; exit writes pack alpha. |
| `shaders/rt/pt_raygen_offline.rgen` | Identical edits (verbatim mirror). |
| `shaders/rt/pt_raygen_realtime.rgen` | Identical edits (no env-MIS divergence for this sub-plan). |
| `examples/env_demo.cpp` | New `--dump-hit-dist-diffuse=<path>` + `--dump-hit-dist-specular=<path>` CLI flags that extract alpha channel from the existing readbacks and encode as grayscale PNG (normalized to max finite). |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append 3.D entry with hit-distance visualizations. |

**No C++ / Vulkan changes.** No new bindings. No new readback helpers (existing `readbackDiffuseRadiance` + `readbackSpecularRadiance` already return all 4 channels per pixel — alpha is readable).

## 6. Verification

On DamagedHelmet + env_studio, 64 spp, `--denoise=none`:

1. **Specular hit-distance dump (`renders/spec_hit_dist_helmet.png`):**
   - Visor + metal body: **bright** (long virtual distance — env reflection is effectively at infinity). For mirror-chain surfaces this is large world-space distance.
   - Matte helmet plates: **dark** (short single-segment distance to nearby geometry).
   - Sky: black (zero alpha, primary miss).

2. **Diffuse hit-distance dump (`renders/diff_hit_dist_helmet.png`):**
   - Matte plates: moderate gray (distance from primary hit to wherever the diffuse bounce-1 ray lands — often a few meters).
   - Visor/metal: black or near-black (metals have `firstHitDiffAlbedo = 0`, so diffContrib is tiny; hit-dist captured but downstream demod makes it irrelevant).
   - Sky: black.

3. **Beauty regression (256 spp):** bit-identical to pre-3.D (we're only writing to alpha channels that `radiance` never reads). Alpha channel change has zero effect on beauty.

4. **AOV sanity:** max specHitDist should be plausible world-space distance (env reflection on mirror → could be 10s or 100s of world units depending on scene extent). Max diffHitDist should be within scene bounds (helmet is ~4 units tall; diffuse bounce hits within scene envelope).

5. **Mandatory "dope render":** full helmet at 256 spp beauty + both hit-distance AOV dumps side-by-side in verification log. User ask: show the pattern clearly.

## 7. Task shape

**Option Y chosen (2 tasks):**

- **Task 1: Shader hit-distance packing (all 3 raygens atomically).** Adds locals, Stage B accumulation, Stage C capture, exit writes. Single commit touches pt_raygen.rgen + pt_raygen_offline.rgen + pt_raygen_realtime.rgen.
- **Task 2: env_demo `--dump-hit-dist-*` CLI + visual verification + log.** Extracts alpha channel from existing readback helpers, encodes as grayscale, appends verification log with helmet observations.

## 8. Risks

| Risk | Mitigation |
|------|-----------|
| Alpha was `1.0`; any consumer that read alpha expecting `1.0` would break | Grep confirmed: only env_demo dumps read these AOVs currently. NRD (Sub-plan 4) expects alpha to BE hit-distance. Change is forward-compatible. |
| RGBA32F alpha precision is plenty for world-space distances up to ~16 million units before precision degrades. Helmet/Cornell scenes are <100 units. | N/A. |
| Specular chain flag break semantics subtle — needs the flag set in the diffuse-sample `else` branch of the bounce-≥2 BSDF sample, NOT in Stage B init | Spec §4.2 explicitly calls out the location. Plan will include the exact line-level edit. |
| Primary miss → alpha 0. NRD interprets 0 as "no sample." First frame may have all-zero AOVs on camera-into-sky — expected. | Document. |
| Diffuse bounce-1 miss (diffuse ray hits sky) → `diffHitRecorded` stays false → `diffHitDist = 0`. NRD still treats 0 as no-sample. | Correct behavior. |

## 9. Out-of-scope (Parking Lot)

- Hit-distance normalization (Sub-plan 4).
- Ping-pong prev-depth (no longer needed).
- Separate hit-distance AOVs (packing into alpha is NRD-canonical; separate AOVs would waste bandwidth).
- Pre-validated hit-distance (e.g. rejecting > scene-diameter values) — NRD's normalization handles this.

## 10. Success Criteria

1. All 3 raygens declare `specHitDist`/`specChainActive`/`diffHitDist`/`diffHitRecorded` locals.
2. Stage B accumulates hit-distance each bounce while `specChainActive == true`; flag flips false on first diffuse sample in bounce-≥2 BSDF branch.
3. Stage C captures bounce-1 hit-distance via `diffHitRecorded` latch.
4. Exit writes pack hit-distance into alpha of bindings 22 and 23.
5. Beauty bit-identical to pre-3.D across `--denoise=none/oidn/optix` (alpha change has no beauty impact).
6. Helmet verification: visor specular-alpha bright (long virtual distance); matte diffuse-alpha moderate; sky both zero.
7. No validation errors, clean build.
8. Verification log updated with hit-distance dumps.

## 11. Next Step

Invoke `superpowers:writing-plans` to generate the 2-task implementation plan.
