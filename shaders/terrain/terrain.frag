#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable

#include "../includes/noise.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// ─── GBuffer MRT outputs ──────────────────────────────────────────────────────
// Attachment 0: VK_FORMAT_R16G16B16A16_SFLOAT  — world pos XYZ + metallic
// Attachment 1: VK_FORMAT_A2R10G10B10_UNORM_PACK32 — oct-encoded normal XY + 0 + roughness
// Attachment 2: VK_FORMAT_R8G8B8A8_SRGB         — albedo RGB + AO
// Attachment 3: VK_FORMAT_R16G16_SFLOAT          — velocity XY (static terrain = 0)
layout(location = 0) out vec4 outGBuffer0;
layout(location = 1) out vec4 outGBuffer1;
layout(location = 2) out vec4 outGBuffer2;
layout(location = 3) out vec2 outVelocity;

// ─── Texture bindings ─────────────────────────────────────────────────────────
layout(set = 0, binding = 0)  uniform sampler2D heightmap;     // not read in frag (used in tese)
layout(set = 0, binding = 1)  uniform sampler2D splatmap;      // RGBA artist weights
layout(set = 0, binding = 2)  uniform sampler2D layer0Albedo;  // grass
layout(set = 0, binding = 3)  uniform sampler2D layer1Albedo;  // dirt
layout(set = 0, binding = 4)  uniform sampler2D layer2Albedo;  // rock
layout(set = 0, binding = 5)  uniform sampler2D layer3Albedo;  // snow
layout(set = 0, binding = 6)  uniform sampler2D layer0Normal;  // grass normal
layout(set = 0, binding = 7)  uniform sampler2D layer1Normal;  // dirt  normal
layout(set = 0, binding = 8)  uniform sampler2D layer2Normal;  // rock  normal
layout(set = 0, binding = 9)  uniform sampler2D layer3Normal;  // snow  normal
layout(set = 0, binding = 10) uniform sampler2D macroVariation; // large-scale color variation

layout(push_constant) uniform TerrainParams {
    mat4    viewProj;
    vec3    cameraPos;
    float   heightScale;
    float   terrainSize;
    float   snowCover;
    float   wetness;
    float   time;
    float   hmapResInv;
    int     terrainType;
    float   pad2;
    float   waterLevel;    //  4 → 112  NEW
    float   frostCover;    //  4 → 116  NEW
    float   tileOffsetX;   //  4 → 120  NEW
    float   tileOffsetZ;   //  4 → 124  NEW
} pc;

// ─── Octahedron normal encoding ───────────────────────────────────────────────
vec2 signNotZero(vec2 v) {
    return vec2((v.x >= 0.0) ? 1.0 : -1.0, (v.y >= 0.0) ? 1.0 : -1.0);
}
vec2 encodeNormal(vec3 n) {
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    return (n.z < 0.0) ? ((1.0 - abs(p.yx)) * signNotZero(p)) : p;
}

// ─── Triplanar sampling ────────────────────────────────────────────────────────
// Blends XZ/XY/ZY projections using the surface normal as weights.
// Avoids texture stretching on steep cliff faces.
vec4 sampleTriplanar(sampler2D tex, vec3 worldPos, vec3 blendW, float scale) {
    vec4 xProj = texture(tex, worldPos.zy * scale);
    vec4 yProj = texture(tex, worldPos.xz * scale);
    vec4 zProj = texture(tex, worldPos.xy * scale);
    return xProj * blendW.x + yProj * blendW.y + zProj * blendW.z;
}

// Normal triplanar (tangent-space normals need axis-specific remapping)
vec3 sampleNormalTriplanar(sampler2D tex, vec3 worldPos, vec3 blendW, float scale) {
    // Sample and decode
    vec3 nx = texture(tex, worldPos.zy * scale).rgb * 2.0 - 1.0;
    vec3 ny = texture(tex, worldPos.xz * scale).rgb * 2.0 - 1.0;
    vec3 nz = texture(tex, worldPos.xy * scale).rgb * 2.0 - 1.0;
    // Swizzle tangent-space normals to world-space before blending
    // (simplified: project onto world axes)
    nx = vec3(nx.z, nx.y, nx.x);   // XZ face: swap XZ
    ny = vec3(ny.x, ny.z, ny.y);   // XY face: swap YZ
    // nz stays as-is
    return normalize(nx * blendW.x + ny * blendW.y + nz * blendW.z);
}

// ─── Height-based layer blending ─────────────────────────────────────────────
// Uses each layer's luminance as a local "micro-height" to drive transition shape.
// Produces natural-looking material boundaries (rock poking through dirt, etc.)
// instead of a linear fade.
vec4 heightBlend(vec4 a, float wa, vec4 b, float wb) {
    // Luminance of each albedo as its local "elevation"
    float ha = dot(a.rgb, vec3(0.299, 0.587, 0.114)) * 0.35 + wa;
    float hb = dot(b.rgb, vec3(0.299, 0.587, 0.114)) * 0.35 + wb;
    float maxH = max(ha, hb) - 0.05;
    float newWa = max(ha - maxH, 0.0);
    float newWb = max(hb - maxH, 0.0);
    float total = newWa + newWb;
    if (total < 0.001) { newWa = wa; newWb = wb; total = max(newWa + newWb, 1e-5); }
    return (a * (newWa / total)) + (b * (newWb / total));
}

// Blend 4 layers using height-based transitions
vec4 blend4(vec4 a, float wa, vec4 b, float wb, vec4 c, float wc, vec4 d, float wd) {
    vec4 ab = heightBlend(a, wa, b, wb);
    float wab = wa + wb;
    vec4 cd = heightBlend(c, wc, d, wd);
    float wcd = wc + wd;
    return heightBlend(ab, wab, cd, wcd);
}

// ─── Anti-tiling: two-scale blend ────────────────────────────────────────────
// Samples at two different scales/offsets and blends. Cheap and very effective.
vec4 sampleAntiTile(sampler2D tex, vec2 uv) {
    // Primary: normal tile
    vec4 s0 = texture(tex, uv);
    // Secondary: 63% scale with half-texel offset — different phase
    vec4 s1 = texture(tex, uv * 0.631 + vec2(0.443, 0.167));
    // Blend by hash-based factor per coarse tile (breaks visible grid)
    float blend = hash21(floor(uv / 4.0)) * 0.3 + 0.15;
    return mix(s0, s1, blend);
}

// Anti-tile variant that also respects triplanar blending
vec4 sampleLayerAT(sampler2D tex, vec2 flatUV, vec3 worldPos, vec3 blendW, float scale,
                   float slopeBlend) {
    // Flat UV sample (anti-tiled)
    vec4 flatSamp = sampleAntiTile(tex, flatUV);
    // Triplanar sample
    vec4 triSamp  = sampleTriplanar(tex, worldPos, blendW, scale);
    return mix(flatSamp, triSamp, slopeBlend);
}

void main() {
    vec3  N      = normalize(inNormal);
    float slope  = 1.0 - N.y;  // 0 = flat, 1 = vertical cliff
    float height01 = (pc.heightScale > 0.0)
                   ? clamp(inWorldPos.y / pc.heightScale, 0.0, 1.0) : 0.0;

    // ─── Triplanar blend weight ──────────────────────────────────────────────
    // Power controls sharpness of the transition (higher = sharper)
    vec3 triW = abs(N);
    triW = pow(triW, vec3(4.0));
    triW /= (triW.x + triW.y + triW.z);
    // How much triplanar to use vs flat UV: ramp from 0 at slope<0.35 to 1 at slope>0.7
    float triBlend = smoothstep(0.35, 0.7, slope);

    // ─── Tile UVs ────────────────────────────────────────────────────────────
    // Detail texture: ~4 m per tile in flat areas
    float tileScale = 1.0 / 4.0;  // world units → UV: 4 m tile
    vec2  tileUV   = inUV * (pc.terrainSize * tileScale);

    // Scale for triplanar (same physical tile size, but in world-space units)
    float triScale = tileScale;

    // ─── Procedural layer weights ─────────────────────────────────────────────
    float grassW = clamp(1.0 - slope * 4.0, 0.0, 1.0);
    float dirtW  = clamp(slope * 3.0 - 0.2, 0.0, 1.0) * (1.0 - step(0.6, slope));
    float rockW  = clamp(slope - 0.4, 0.0, 1.0);
    float snowW  = clamp((height01 - 0.7) * 5.0 + pc.snowCover, 0.0, 1.0);
    // Snow only settles on near-flat surfaces
    snowW *= clamp(1.0 - slope * 3.0, 0.0, 1.0);

    // Normalize procedural weights
    float total = grassW + dirtW + rockW + snowW + 1e-5;
    grassW /= total; dirtW /= total; rockW /= total; snowW /= total;

    // ─── Splatmap override ────────────────────────────────────────────────────
    vec4  splat    = texture(splatmap, inUV);
    float splatSum = splat.r + splat.g + splat.b + splat.a;
    if (splatSum > 0.01) {
        float invS = 1.0 / max(splatSum, 1e-5);
        grassW = splat.r * invS;
        dirtW  = splat.g * invS;
        rockW  = splat.b * invS;
        snowW  = splat.a * invS;
    }

    // ─── Sample layer albedos (anti-tiled + triplanar on cliffs) ─────────────
    vec4 aGrass = sampleLayerAT(layer0Albedo, tileUV, inWorldPos, triW, triScale, triBlend);
    vec4 aDirt  = sampleLayerAT(layer1Albedo, tileUV, inWorldPos, triW, triScale, triBlend);
    vec4 aRock  = sampleLayerAT(layer2Albedo, tileUV, inWorldPos, triW, triScale, triBlend);
    vec4 aSnow  = sampleLayerAT(layer3Albedo, tileUV, inWorldPos, triW, triScale, triBlend);

    // ─── Height-blend albedo ──────────────────────────────────────────────────
    vec4 albedo = blend4(aGrass, grassW, aDirt, dirtW, aRock, rockW, aSnow, snowW);

    // ─── Macro variation ──────────────────────────────────────────────────────
    // Large-scale (~64 m) color variation breaks repetition at distance.
    float macroScale = 1.0 / 64.0;
    vec2  macroUV    = inUV * (pc.terrainSize * macroScale);
    vec4  macro      = texture(macroVariation, macroUV);
    // Blend: multiply at 60% strength (too strong looks painted)
    albedo.rgb = mix(albedo.rgb, albedo.rgb * macro.rgb * 2.0, 0.35);
    albedo.rgb = clamp(albedo.rgb, 0.0, 1.0);

    // ─── Wetness darkening ────────────────────────────────────────────────────
    float wetFactor = pc.wetness * (1.0 - snowW) * 0.4;
    albedo.rgb *= (1.0 - wetFactor);

    // ─── Shore transition ─────────────────────────────────────────────────────
    // Within 1.5 m above waterLevel: gradually blend to wet sand/silt material.
    // Below waterLevel we discard (the water pass covers it).
    if (inWorldPos.y < pc.waterLevel) { discard; }
    float shoreT = clamp(1.0 - (inWorldPos.y - pc.waterLevel) / 1.5, 0.0, 1.0);
    if (shoreT > 0.001) {
        // Wet dark sand color; frost brightens it slightly
        vec3 sandBase  = vec3(0.48, 0.40, 0.28);
        vec3 sandColor = mix(sandBase, sandBase * 1.4, pc.frostCover * 0.4);
        sandColor     *= (1.0 - wetFactor * 0.6);  // wet sand is darker
        albedo.rgb     = mix(albedo.rgb, sandColor, shoreT * 0.75);
        // roughness modified below after it is declared in the PBR block
    }

    // ─── Normal maps (anti-tiled + triplanar) ────────────────────────────────
    vec3 dn0, dn1, dn2, dn3;
    if (triBlend < 0.01) {
        // Pure flat UV (common case — cheap)
        dn0 = sampleAntiTile(layer0Normal, tileUV).rgb * 2.0 - 1.0;
        dn1 = sampleAntiTile(layer1Normal, tileUV).rgb * 2.0 - 1.0;
        dn2 = sampleAntiTile(layer2Normal, tileUV).rgb * 2.0 - 1.0;
        dn3 = sampleAntiTile(layer3Normal, tileUV).rgb * 2.0 - 1.0;
    } else {
        // Triplanar normals on steep slopes
        dn0 = sampleNormalTriplanar(layer0Normal, inWorldPos, triW, triScale);
        dn1 = sampleNormalTriplanar(layer1Normal, inWorldPos, triW, triScale);
        dn2 = sampleNormalTriplanar(layer2Normal, inWorldPos, triW, triScale);
        dn3 = sampleNormalTriplanar(layer3Normal, inWorldPos, triW, triScale);
        // Flat UV blended in for partial slopes
        if (triBlend < 0.99) {
            vec3 fn0 = sampleAntiTile(layer0Normal, tileUV).rgb * 2.0 - 1.0;
            vec3 fn1 = sampleAntiTile(layer1Normal, tileUV).rgb * 2.0 - 1.0;
            vec3 fn2 = sampleAntiTile(layer2Normal, tileUV).rgb * 2.0 - 1.0;
            vec3 fn3 = sampleAntiTile(layer3Normal, tileUV).rgb * 2.0 - 1.0;
            dn0 = mix(fn0, dn0, triBlend);
            dn1 = mix(fn1, dn1, triBlend);
            dn2 = mix(fn2, dn2, triBlend);
            dn3 = mix(fn3, dn3, triBlend);
        }
    }

    // Blend detail normals by layer weights (simple weighted sum, then normalize)
    vec3 detailN = normalize(
          grassW * dn0 + dirtW * dn1 + rockW * dn2 + snowW * dn3);

    // Whiteout blend: combine geometry normal with detail tangent-space normal
    // Produces better results than naive linear mix.
    float detailStrength = mix(0.25, 0.45, triBlend);  // stronger on cliffs
    vec3 blendedN = normalize(vec3(
        N.x + detailN.x * detailStrength,
        N.y,
        N.z + detailN.y * detailStrength));

    // ── Snow normal smoothing ─────────────────────────────────────────────────
    // Snow fills surface crevices → blend toward world-up as snow accumulates.
    // Heavy snow (snowW * snowCover near 1) → nearly flat surface normals.
    blendedN = normalize(mix(blendedN, vec3(0.0, 1.0, 0.0), snowW * pc.snowCover * 0.6));

    // ─── PBR properties ───────────────────────────────────────────────────────
    float metallic  = 0.0;
    float roughness = mix(0.85, 0.55, pc.wetness * (1.0 - snowW));
    roughness       = mix(roughness, 0.95, snowW);
    // Rock is slightly less rough than default soil
    roughness       = mix(roughness, 0.75, rockW * 0.4);
    // Shore zone: wet sand is much smoother than dry terrain
    roughness       = mix(roughness, 0.35, shoreT * 0.65);

    // ── Cavity AO from heightmap curvature ───────────────────────────────────
    // Laplacian of the heightmap: positive = convex ridge (bright),
    // negative = concave valley/cavity (dark, shadowed by surrounding terrain).
    float ts2 = pc.hmapResInv * 2.0;
    float hC  = texture(heightmap, inUV).r;
    float hN  = texture(heightmap, inUV + vec2(0.0,  ts2)).r;
    float hS  = texture(heightmap, inUV + vec2(0.0, -ts2)).r;
    float hE  = texture(heightmap, inUV + vec2( ts2, 0.0)).r;
    float hW  = texture(heightmap, inUV + vec2(-ts2, 0.0)).r;
    float laplacian = 4.0 * hC - (hN + hS + hE + hW);  // >0=convex, <0=concave
    // Map: concave valleys darkened; convex ridges slightly brightened.
    float cavityAO = clamp(1.0 + laplacian * 10.0, 0.25, 1.15);
    // Snow fills cavities → reduce cavity effect under snow
    cavityAO = mix(cavityAO, 1.0, snowW * pc.snowCover);
    float ao = clamp(cavityAO, 0.0, 1.0);

    // ─── Write GBuffer ────────────────────────────────────────────────────────
    outGBuffer0 = vec4(inWorldPos, metallic);
    outGBuffer1 = vec4(encodeNormal(blendedN), 0.0, roughness);
    outGBuffer2 = vec4(albedo.rgb, ao);
    outVelocity = vec2(0.0);  // static terrain
}
