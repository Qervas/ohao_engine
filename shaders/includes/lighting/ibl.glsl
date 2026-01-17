#ifndef IBL_GLSL
#define IBL_GLSL

// Image-Based Lighting Sampling Functions
// For use with split-sum approximation

const float PI = 3.14159265359;
const float MAX_REFLECTION_LOD = 4.0;

// Fresnel-Schlick with roughness term for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Sample irradiance map for diffuse IBL
vec3 sampleIrradiance(samplerCube irradianceMap, vec3 N) {
    return texture(irradianceMap, N).rgb;
}

// Sample prefiltered environment map for specular IBL
vec3 samplePrefilteredEnvMap(samplerCube prefilteredMap, vec3 R, float roughness) {
    float lod = roughness * MAX_REFLECTION_LOD;
    return textureLod(prefilteredMap, R, lod).rgb;
}

// Sample BRDF LUT
vec2 sampleBRDF_LUT(sampler2D brdfLUT, float NdotV, float roughness) {
    return texture(brdfLUT, vec2(NdotV, roughness)).rg;
}

// Calculate IBL contribution
// F0: Base reflectivity
// roughness: Surface roughness [0, 1]
// metallic: Metalness [0, 1]
// albedo: Surface color
// N: Surface normal
// V: View direction
vec3 calculateIBL(
    samplerCube irradianceMap,
    samplerCube prefilteredMap,
    sampler2D brdfLUT,
    vec3 F0,
    float roughness,
    float metallic,
    vec3 albedo,
    vec3 N,
    vec3 V,
    float ao
) {
    float NdotV = max(dot(N, V), 0.0);
    vec3 R = reflect(-V, N);

    // Fresnel with roughness
    vec3 F = fresnelSchlickRoughness(NdotV, F0, roughness);

    // kS is the specular contribution
    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic; // Metals have no diffuse

    // Diffuse IBL
    vec3 irradiance = sampleIrradiance(irradianceMap, N);
    vec3 diffuse = irradiance * albedo;

    // Specular IBL
    vec3 prefilteredColor = samplePrefilteredEnvMap(prefilteredMap, R, roughness);
    vec2 brdf = sampleBRDF_LUT(brdfLUT, NdotV, roughness);
    vec3 specular = prefilteredColor * (F * brdf.x + brdf.y);

    // Combine
    vec3 ambient = (kD * diffuse + specular) * ao;
    return ambient;
}

// Simplified IBL for performance-critical paths
vec3 calculateIBL_Simple(
    samplerCube envMap,
    sampler2D brdfLUT,
    vec3 F0,
    float roughness,
    float metallic,
    vec3 albedo,
    vec3 N,
    vec3 V,
    float ao
) {
    float NdotV = max(dot(N, V), 0.0);
    vec3 R = reflect(-V, N);

    vec3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    // Sample environment at multiple roughness levels
    float lod = roughness * MAX_REFLECTION_LOD;

    // Use lowest mip for diffuse approximation
    vec3 irradiance = textureLod(envMap, N, MAX_REFLECTION_LOD).rgb * 0.3;
    vec3 diffuse = irradiance * albedo;

    // Specular from prefiltered environment
    vec3 prefilteredColor = textureLod(envMap, R, lod).rgb;
    vec2 brdf = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    vec3 specular = prefilteredColor * (F * brdf.x + brdf.y);

    return (kD * diffuse + specular) * ao;
}

#endif // IBL_GLSL
