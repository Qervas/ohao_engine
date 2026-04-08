// cloud_density.glsl — Guerrilla Games-style cloud density (Schneider, SIGGRAPH 2015)
//
// Anti-tiling pipeline:
// 1. Spherical altitude (not worldPos.y) for height gradient
// 2. Low-frequency UV warp (8-12x lower than base) to break grid alignment
// 3. Non-harmonic dual-layer base noise (1x + 2.37x) — LCM is enormous
// 4. Multi-scale weather map sampling (2 scales at non-harmonic ratio)
// 5. Remap-based density (Schneider) + detail erosion

#ifndef OHAO_CLOUD_DENSITY_GLSL
#define OHAO_CLOUD_DENSITY_GLSL

// Earth geometry (must match cloud.comp)
const float CLOUD_EARTH_RADIUS = 6371000.0;
const vec3  CLOUD_EARTH_CENTER = vec3(0.0, -CLOUD_EARTH_RADIUS, 0.0);

// ---------------------------------------------------------------------------
// Remap: maps value from [oldMin, oldMax] to [newMin, newMax], clamped
// ---------------------------------------------------------------------------
float cloudRemap(float value, float oldMin, float oldMax, float newMin, float newMax) {
    return clamp(newMin + ((value - oldMin) / max(oldMax - oldMin, 1e-5)) * (newMax - newMin),
                 min(newMin, newMax), max(newMin, newMax));
}

// ---------------------------------------------------------------------------
// Height gradient for cloud types (Schneider, SIGGRAPH 2015)
// cloudType: 0 = stratus, 0.5 = stratocumulus, 1.0 = cumulus
// ---------------------------------------------------------------------------
float densityHeightGradient(float h, float cloudType) {
    const vec4 STRATUS       = vec4(0.00, 0.10, 0.20, 0.30);
    const vec4 STRATOCUMULUS = vec4(0.02, 0.20, 0.48, 0.625);
    const vec4 CUMULUS       = vec4(0.00, 0.1625, 0.88, 0.98);

    float sw = 1.0 - clamp(cloudType * 2.0, 0.0, 1.0);
    float scw = 1.0 - abs(cloudType - 0.5) * 2.0;
    float cw = clamp(cloudType - 0.5, 0.0, 1.0) * 2.0;

    vec4 g = STRATUS * sw + STRATOCUMULUS * scw + CUMULUS * cw;
    return smoothstep(g.x, g.y, h) - smoothstep(g.z, g.w, h);
}

// ---------------------------------------------------------------------------
// Sample cloud density at world-space position
//
// cheapSample: skip detail erosion (for shadow/light rays — 3x cheaper)
// ---------------------------------------------------------------------------
float sampleCloudDensity(
    vec3 worldPos,
    sampler3D noiseTexture,
    sampler2D weatherMap,
    float altMin, float altMax,
    float coverage, float densityScale,
    float time, float windSpeed,
    bool cheapSample
) {
    // --- Spherical altitude (not worldPos.y) ---
    // Fixes horizontal band artifacts when looking straight up
    float posAlt = length(worldPos - CLOUD_EARTH_CENTER) - CLOUD_EARTH_RADIUS;
    float h = clamp((posAlt - altMin) / max(altMax - altMin, 1.0), 0.0, 1.0);

    // --- Multi-scale weather map (breaks 2D tiling on XZ plane) ---
    // Large scale: storm systems (~80km per tile)
    vec2 wUVLarge = worldPos.xz * 0.0000125 + vec2(time * windSpeed * 0.00002, 0.0);
    vec4 wLarge = texture(weatherMap, wUVLarge);

    // Medium scale: local variation (~26km per tile, ratio 3.04x — non-harmonic)
    vec2 wUVMed = worldPos.xz * 0.000038 + vec2(time * windSpeed * 0.000055, time * windSpeed * 0.00001);
    vec4 wMed = texture(weatherMap, wUVMed);

    // Blend: large drives shape, medium adds variation that hides large-scale seams
    float wCoverage = clamp(wLarge.r * 0.7 + wMed.r * 0.4, 0.0, 1.0);
    float cloudType = wLarge.g * 0.8 + wMed.g * 0.2;

    // Height gradient based on cloud type
    float heightGrad = densityHeightGradient(h, cloudType);
    if (heightGrad < 0.01) return 0.0;

    // Wind animation
    vec3 windOffset = vec3(time * windSpeed * 0.00015, 0.0, time * windSpeed * 0.00005);

    // --- Base noise UV (128^3 RGBA8, ~67km per tile at 0.000015) ---
    vec3 baseUVW = worldPos * 0.000015 + windOffset;

    // --- Low-frequency UV warp (8x lower = ~555km per warp tile) ---
    // Must be at a very different scale to actually decorrelate from base
    vec3 warpPos = worldPos * 0.0000018 + windOffset * 0.05;
    vec3 warpRaw = texture(noiseTexture, warpPos).gba;
    vec3 warp = (warpRaw - 0.5) * 0.10; // ~6.6km displacement in world
    baseUVW += warp;

    // --- Dual-layer base noise (non-harmonic ratio breaks tiling) ---
    vec4 noise1 = texture(noiseTexture, baseUVW);

    // Second layer at 2.37x scale — incommensurable ratio means
    // the combined pattern's repeat period is effectively infinite
    vec3 baseUVW2 = worldPos * 0.0000356 + windOffset * 1.3;
    // Its own warp at a different scale
    vec3 warp2Raw = texture(noiseTexture, worldPos * 0.0000025 + 0.5).gba;
    baseUVW2 += (warp2Raw - 0.5) * 0.08;
    vec4 noise2 = texture(noiseTexture, baseUVW2);

    // Blend: first layer dominates, second breaks the repeat
    vec4 noise = noise1 * 0.65 + noise2 * 0.35;

    // Build Worley FBM from GBA channels
    float worleyFBM = noise.g * 0.625 + noise.b * 0.250 + noise.a * 0.125;

    // BASE SHAPE: remap R channel using Worley FBM as lower bound
    float baseCloud = cloudRemap(noise.r, -(1.0 - worleyFBM), 1.0, 0.0, 1.0);

    // Apply height gradient
    baseCloud *= heightGrad;

    // Apply coverage from weather map + push constant
    float totalCoverage = clamp(coverage * wCoverage * 2.0, 0.02, 1.0);
    baseCloud = cloudRemap(baseCloud, 1.0 - totalCoverage, 1.0, 0.0, 1.0);
    baseCloud *= totalCoverage;

    if (baseCloud <= 0.0) return 0.0;

    // Cheap sample: skip detail erosion (for shadow rays)
    if (cheapSample) return clamp(baseCloud * densityScale, 0.0, 1.0);

    // --- Detail erosion (non-harmonic ratio 4.7x from base) ---
    vec3 uvwDetail = worldPos * 0.0000705 + windOffset * 0.15;
    // Small warp for detail too
    vec3 detWarpRaw = texture(noiseTexture, uvwDetail * 0.25 + 0.3).gba;
    uvwDetail += (detWarpRaw - 0.5) * 0.08;

    vec3 detailNoise = texture(noiseTexture, uvwDetail).gba;
    float detailFBM = detailNoise.x * 0.625 + detailNoise.y * 0.250 + detailNoise.z * 0.125;

    // Invert erosion direction based on height:
    // Low (h→0): wispy underside; High (h→1): crisp top edge
    float detailMod = mix(1.0 - detailFBM, detailFBM, clamp(h * 5.0, 0.0, 1.0));

    // Erode using remap (preserves interior while carving edges)
    float finalCloud = cloudRemap(baseCloud, detailMod * 0.35, 1.0, 0.0, 1.0);

    return clamp(finalCloud * densityScale, 0.0, 1.0);
}

#endif // OHAO_CLOUD_DENSITY_GLSL
