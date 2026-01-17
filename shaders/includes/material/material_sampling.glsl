// material_sampling.glsl - Texture sampling functions for PBR materials
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/material/material_sampling.glsl
//
// Provides functions for sampling PBR material textures.
// Requires: material_types.glsl

#ifndef OHAO_MATERIAL_SAMPLING_GLSL
#define OHAO_MATERIAL_SAMPLING_GLSL

#include "includes/material/material_types.glsl"
#include "includes/common/color.glsl"

// =============================================================================
// Texture Coordinate Transformation
// =============================================================================

// Apply UV scale and offset from material
vec2 transformTexCoord(vec2 texCoord, MaterialData material) {
    return texCoord * material.uvScale + material.uvOffset;
}

// =============================================================================
// Normal Map Sampling
// =============================================================================

// Sample and decode normal from normal map
// Uses tangent-space to world-space transformation
// Parameters:
//   normalMap: normal map texture sampler
//   texCoord: texture coordinates
//   worldNormal: interpolated vertex normal
//   worldTangent: interpolated vertex tangent
//   worldBitangent: interpolated vertex bitangent
//   normalScale: normal map intensity
// Returns: world-space normal
vec3 sampleNormalMap(sampler2D normalMap, vec2 texCoord,
                      vec3 worldNormal, vec3 worldTangent, vec3 worldBitangent,
                      float normalScale) {
    // Sample normal map (assumed to be in tangent space)
    vec3 tangentNormal = texture(normalMap, texCoord).rgb;

    // Unpack from [0,1] to [-1,1]
    tangentNormal = tangentNormal * 2.0 - 1.0;

    // Apply normal scale (intensity)
    tangentNormal.xy *= normalScale;
    tangentNormal = normalize(tangentNormal);

    // Build TBN matrix (tangent-space to world-space)
    mat3 TBN = mat3(
        normalize(worldTangent),
        normalize(worldBitangent),
        normalize(worldNormal)
    );

    // Transform to world space
    return normalize(TBN * tangentNormal);
}

// Compute TBN matrix from derivatives (when tangents are not available)
// This uses screen-space derivatives to compute tangent and bitangent
mat3 computeTBN(vec3 worldPos, vec3 worldNormal, vec2 texCoord) {
    // Get edge vectors of the pixel triangle
    vec3 dp1 = dFdx(worldPos);
    vec3 dp2 = dFdy(worldPos);
    vec2 duv1 = dFdx(texCoord);
    vec2 duv2 = dFdy(texCoord);

    // Solve the linear system
    vec3 dp2perp = cross(dp2, worldNormal);
    vec3 dp1perp = cross(worldNormal, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    // Construct a scale-invariant TBN
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    return mat3(T * invmax, B * invmax, worldNormal);
}

// Sample normal map using computed TBN (no vertex tangents)
vec3 sampleNormalMapNoTangent(sampler2D normalMap, vec2 texCoord,
                               vec3 worldPos, vec3 worldNormal, float normalScale) {
    vec3 tangentNormal = texture(normalMap, texCoord).rgb;
    tangentNormal = tangentNormal * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    tangentNormal = normalize(tangentNormal);

    mat3 TBN = computeTBN(worldPos, worldNormal, texCoord);
    return normalize(TBN * tangentNormal);
}

// =============================================================================
// Albedo / Base Color Sampling
// =============================================================================

// Sample albedo texture and convert from sRGB to linear
vec4 sampleAlbedo(sampler2D albedoMap, vec2 texCoord) {
    vec4 albedo = texture(albedoMap, texCoord);
    // Albedo textures are typically stored in sRGB
    albedo.rgb = sRGBToLinear(albedo.rgb);
    return albedo;
}

// =============================================================================
// Metallic-Roughness Sampling
// =============================================================================

// Sample metallic-roughness texture
// glTF convention: R = unused, G = roughness, B = metallic
// Some engines use: R = metallic, G = roughness
vec2 sampleMetallicRoughness(sampler2D metallicRoughnessMap, vec2 texCoord) {
    vec4 mr = texture(metallicRoughnessMap, texCoord);
    // Using glTF convention
    return vec2(mr.b, mr.g); // (metallic, roughness)
}

// =============================================================================
// Ambient Occlusion Sampling
// =============================================================================

// Sample ambient occlusion texture
// AO maps are typically single-channel stored in R
float sampleAO(sampler2D aoMap, vec2 texCoord, float occlusionStrength) {
    float ao = texture(aoMap, texCoord).r;
    // Apply occlusion strength (1.0 = full effect, 0.0 = no effect)
    return mix(1.0, ao, occlusionStrength);
}

// =============================================================================
// Emissive Sampling
// =============================================================================

// Sample emissive texture and convert from sRGB to linear
vec3 sampleEmissive(sampler2D emissiveMap, vec2 texCoord, float intensity) {
    vec3 emissive = texture(emissiveMap, texCoord).rgb;
    emissive = sRGBToLinear(emissive);
    return emissive * intensity;
}

// =============================================================================
// Complete Material Evaluation
// =============================================================================

// Evaluate all material textures and populate surface data
// This is a template function - actual implementation depends on
// how textures are bound in your descriptor set.
//
// Example usage:
// void evaluateMaterial(inout SurfaceData surface, MaterialData material,
//                       sampler2D[] textures, vec3 worldTangent, vec3 worldBitangent) {
//     vec2 uv = transformTexCoord(surface.texCoord, material);
//
//     // Albedo
//     if (hasFlag(material, MATERIAL_FLAG_HAS_ALBEDO_MAP)) {
//         vec4 albedoSample = sampleAlbedo(textures[material.albedoMapIndex], uv);
//         surface.albedo = albedoSample.rgb * material.baseColor.rgb;
//         surface.alpha = albedoSample.a * material.baseColor.a;
//     } else {
//         surface.albedo = material.baseColor.rgb;
//         surface.alpha = material.baseColor.a;
//     }
//
//     // Normal map
//     if (hasFlag(material, MATERIAL_FLAG_HAS_NORMAL_MAP)) {
//         surface.normal = sampleNormalMap(textures[material.normalMapIndex], uv,
//                                          surface.geometryNormal, worldTangent,
//                                          worldBitangent, material.normalScale);
//     }
//
//     // Metallic-Roughness
//     if (hasFlag(material, MATERIAL_FLAG_HAS_METALLIC_ROUGHNESS_MAP)) {
//         vec2 mr = sampleMetallicRoughness(textures[material.metallicRoughnessMapIndex], uv);
//         surface.metallic = mr.x * material.metallic;
//         surface.roughness = mr.y * material.roughness;
//     } else {
//         surface.metallic = material.metallic;
//         surface.roughness = material.roughness;
//     }
//
//     // Ambient Occlusion
//     if (hasFlag(material, MATERIAL_FLAG_HAS_AO_MAP)) {
//         surface.ao = sampleAO(textures[material.aoMapIndex], uv, material.occlusionStrength);
//     } else {
//         surface.ao = material.ao;
//     }
//
//     // Emissive
//     if (hasFlag(material, MATERIAL_FLAG_HAS_EMISSIVE_MAP)) {
//         surface.emissive = sampleEmissive(textures[material.emissiveMapIndex], uv,
//                                           material.emissive.w);
//     } else {
//         surface.emissive = material.emissive.rgb * material.emissive.w;
//     }
//
//     // Calculate F0
//     surface.F0 = calculateF0(surface);
//
//     // Apply double-sided shading if needed
//     if (hasFlag(material, MATERIAL_FLAG_DOUBLE_SIDED)) {
//         applyDoubleSided(surface);
//     }
// }

#endif // OHAO_MATERIAL_SAMPLING_GLSL
