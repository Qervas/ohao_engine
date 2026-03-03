// OHAO Shared Noise Library — noise.glsl
// Include with: #extension GL_GOOGLE_include_directive : enable
//               #include "includes/noise.glsl"
//
// All functions operate in 2D for terrain use. 3D variants where noted.
// Coordinate conventions match GLSL (right-handed, Y-up).

#ifndef OHAO_NOISE_GLSL
#define OHAO_NOISE_GLSL

// ─── Hash Functions ────────────────────────────────────────────────────────────

// float → float  [0, 1)
float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

// vec2 → float  [0, 1)
float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// vec2 → vec2  [0, 1)²
vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

// vec3 → float  [0, 1)
float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// vec3 → vec3  [0, 1)³
vec3 hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// ─── Value Noise ───────────────────────────────────────────────────────────────

// Smooth value noise 2D → [0, 1]
float valueNoise2D(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);  // Hermite smoothstep
    return mix(
        mix(hash21(i),               hash21(i + vec2(1,0)), u.x),
        mix(hash21(i + vec2(0,1)),   hash21(i + vec2(1,1)), u.x), u.y);
}

// ─── Gradient Noise (Perlin-style) ────────────────────────────────────────────

// 2D gradient noise → [-1, 1]
float gradientNoise2D(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    // Quintic smoothstep (C2 continuity)
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    vec2 g00 = hash22(i)               * 2.0 - 1.0;
    vec2 g10 = hash22(i + vec2(1,0))   * 2.0 - 1.0;
    vec2 g01 = hash22(i + vec2(0,1))   * 2.0 - 1.0;
    vec2 g11 = hash22(i + vec2(1,1))   * 2.0 - 1.0;

    float n00 = dot(g00, f);
    float n10 = dot(g10, f - vec2(1,0));
    float n01 = dot(g01, f - vec2(0,1));
    float n11 = dot(g11, f - vec2(1,1));

    return mix(mix(n00, n10, u.x), mix(n01, n11, u.x), u.y);
}

// 3D gradient noise → [-1, 1]
float gradientNoise3D(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float n000 = dot(hash33(i)               * 2.0 - 1.0, f);
    float n100 = dot(hash33(i + vec3(1,0,0)) * 2.0 - 1.0, f - vec3(1,0,0));
    float n010 = dot(hash33(i + vec3(0,1,0)) * 2.0 - 1.0, f - vec3(0,1,0));
    float n110 = dot(hash33(i + vec3(1,1,0)) * 2.0 - 1.0, f - vec3(1,1,0));
    float n001 = dot(hash33(i + vec3(0,0,1)) * 2.0 - 1.0, f - vec3(0,0,1));
    float n101 = dot(hash33(i + vec3(1,0,1)) * 2.0 - 1.0, f - vec3(1,0,1));
    float n011 = dot(hash33(i + vec3(0,1,1)) * 2.0 - 1.0, f - vec3(0,1,1));
    float n111 = dot(hash33(i + vec3(1,1,1)) * 2.0 - 1.0, f - vec3(1,1,1));

    return mix(
        mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
        mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

// ─── fBm (Fractional Brownian Motion) ────────────────────────────────────────

// fBm 2D → approximately [-1, 1] (actual range depends on octaves)
float fbm2D(vec2 p, int octaves, float persistence, float lacunarity) {
    float value    = 0.0;
    float amplitude = 0.5;
    float maxValue  = 0.0;
    float frequency = 1.0;

    for (int i = 0; i < octaves; i++) {
        value    += gradientNoise2D(p * frequency) * amplitude;
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }
    return value / maxValue;  // normalize to roughly [-1, 1]
}

// fBm with rotation per octave (reduces axis-aligned artifacts)
float fbm2DRotated(vec2 p, int octaves, float persistence, float lacunarity) {
    // 37.3° rotation matrix
    const mat2 rot = mat2(0.7964, 0.6048, -0.6048, 0.7964);
    float value    = 0.0;
    float amplitude = 0.5;
    float maxValue  = 0.0;

    for (int i = 0; i < octaves; i++) {
        value    += gradientNoise2D(p) * amplitude;
        maxValue += amplitude;
        p        *= rot * lacunarity;
        amplitude *= persistence;
    }
    return value / maxValue;
}

// ─── Ridged Multifractal ──────────────────────────────────────────────────────

// Ridged noise → [0, 1]  (peaks at ridgeOffset - mountain crests)
float ridgedNoise2D(vec2 p, int octaves, float persistence, float lacunarity, float ridgeOffset) {
    float value    = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    float weight    = 1.0;

    for (int i = 0; i < octaves; i++) {
        float signal  = gradientNoise2D(p * frequency);
        signal = ridgeOffset - abs(signal);   // fold to create ridges
        signal *= signal * weight;            // sharpen and weight by previous
        weight  = clamp(signal * 2.0, 0.0, 1.0);
        value  += signal * amplitude;
        frequency *= lacunarity;
        amplitude *= persistence;
    }
    return clamp(value, 0.0, 1.0);
}

// ─── Domain Warping ───────────────────────────────────────────────────────────

// Double-warp fBm (Inigo Quilez style) → [-1, 1]
// Creates complex eroded/canyon-like structures.
float warpedNoise2D(vec2 p, int octaves, float persistence, float lacunarity, float warpStrength) {
    // First warp field
    vec2 q = vec2(
        fbm2D(p,                   4, persistence, lacunarity),
        fbm2D(p + vec2(5.2, 1.3), 4, persistence, lacunarity));

    // Second warp (warp the already-warped domain)
    vec2 r = vec2(
        fbm2D(p + warpStrength * q + vec2(1.7, 9.2), 4, persistence, lacunarity),
        fbm2D(p + warpStrength * q + vec2(8.3, 2.8), 4, persistence, lacunarity));

    return fbm2D(p + warpStrength * r, octaves, persistence, lacunarity);
}

// ─── Voronoi / Worley Noise ───────────────────────────────────────────────────

// Returns vec2(dist_to_nearest, dist_to_2nd_nearest) → both in [0, ~1]
vec2 voronoi2D(vec2 p) {
    vec2 i  = floor(p);
    vec2 f  = fract(p);
    float d1 = 1e10, d2 = 1e10;

    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            vec2 cell  = vec2(float(x), float(y));
            vec2 point = hash22(i + cell);  // random point within cell
            vec2 diff  = cell + point - f;
            float dist = dot(diff, diff);
            if (dist < d1) { d2 = d1; d1 = dist; }
            else if (dist < d2) { d2 = dist; }
        }
    }
    return vec2(sqrt(d1), sqrt(d2));
}

// Cell borders (d2 - d1) → great for crack patterns
float voronoiEdge2D(vec2 p) {
    vec2 v = voronoi2D(p);
    return v.y - v.x;
}

// ─── Dune Noise ───────────────────────────────────────────────────────────────

// Directional dune profile for desert terrain → [0, 1]
float duneNoise2D(vec2 p, vec2 windDir, float frequency, int octaves) {
    windDir       = normalize(windDir);
    vec2 perp     = vec2(-windDir.y, windDir.x);
    float value   = 0.0;
    float amplitude = 1.0;
    float freq      = frequency;
    float norm      = 0.0;

    for (int i = 0; i < octaves; i++) {
        float along = dot(p, windDir) * freq;
        float across = dot(p, perp)  * freq * 0.25;
        // Asymmetric sawtooth: gentle windward slope, sharp leeward face
        float dune = sin(along + 1.8 * sin(along * 0.55) + across * 0.4);
        dune = dune * 0.5 + 0.5;
        dune = pow(dune, 1.8);  // sharpen crests
        value += dune * amplitude;
        norm  += amplitude;
        amplitude *= 0.45;
        freq      *= 2.1;
    }
    return value / norm;
}

// ─── Utility ──────────────────────────────────────────────────────────────────

// Simple anti-tiling: sample at two scales with hash offset to break repetition
// Call on tileUV before sampling your texture. Cost: 1 hash + minor math.
vec2 antiTileUV(vec2 uv, float tileFreq) {
    vec2 cell = floor(uv / tileFreq);
    float angle = hash21(cell) * 6.28318;
    float c = cos(angle), s = sin(angle);
    vec2  center = (cell + 0.5) * tileFreq;
    return mat2(c, -s, s, c) * (uv - center) + center;
}

// Stochastic texture fetch: blends two rotated instances to hide tiling
// More expensive than antiTileUV but provides seamless results.
vec4 sampleStochastic(sampler2D tex, vec2 uv) {
    vec2 iuv = floor(uv);
    vec2 fuv = fract(uv);

    // Hash for this integer tile
    float h  = hash21(iuv);
    float angle = h * 6.28318;
    float c = cos(angle), s = sin(angle);
    mat2  rot = mat2(c, -s, s, c);

    // Primary: rotated sample
    vec2 uv1 = rot * (fuv - 0.5) + 0.5 + iuv;
    // Secondary: original (slight offset to avoid seam)
    vec2 uv2 = uv;

    // Blend factor based on distance to cell center (smooth at edges)
    float blend = smoothstep(0.4, 0.6, length(fuv - 0.5) * 2.0);
    return mix(texture(tex, uv1), texture(tex, uv2), blend * 0.35);
}

#endif // OHAO_NOISE_GLSL
