// blinn_phong.glsl - Blinn-Phong lighting calculation functions
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/lighting/blinn_phong.glsl
//
// Requires: includes/common/types.glsl
// IMPORTANT: Caller must define LightUBO named 'lighting' before including this file!

#ifndef OHAO_LIGHTING_BLINN_PHONG_GLSL
#define OHAO_LIGHTING_BLINN_PHONG_GLSL

// Calculate attenuation for point/spot lights
// Uses smooth quadratic falloff with range-based cutoff
float calculateAttenuation(float distance, float range) {
    // Quadratic falloff with range
    float attenuation = 1.0 / (1.0 + (distance * distance) / (range * range));
    // Smooth edge falloff
    float edgeFactor = clamp(1.0 - distance / range, 0.0, 1.0);
    return attenuation * edgeFactor * edgeFactor;
}

// Calculate Blinn-Phong light contribution
// CRITICAL: Pass light INDEX, not Light struct, to avoid GLSL struct copy corruption!
// Parameters:
//   lightIndex - Index into lighting.lights[] array
//   fragPos    - Fragment world position
//   normal     - Fragment surface normal (normalized)
//   viewDir    - Direction from fragment to camera (normalized)
//   baseColor  - Surface base color (albedo)
//   shadow     - Shadow factor (0.0 = lit, 1.0 = shadowed)
//   shininess  - Specular shininess exponent (typically 32-256)
// Returns: Combined diffuse + specular contribution with shadow applied
vec3 calculateBlinnPhongForLightIndex(int lightIndex, vec3 fragPos, vec3 normal, vec3 viewDir,
                                       vec3 baseColor, float shadow, float shininess) {
    // Access UBO members DIRECTLY - do NOT copy the Light struct!
    int lightType = int(lighting.lights[lightIndex].position.w);
    vec3 lightColor = lighting.lights[lightIndex].color.rgb;
    float intensity = lighting.lights[lightIndex].color.w;
    float range = lighting.lights[lightIndex].direction.w;

    vec3 lightDir;
    float attenuation = 1.0;

    if (lightType == LIGHT_DIRECTIONAL) {
        lightDir = normalize(-lighting.lights[lightIndex].direction.xyz);
    }
    else if (lightType == LIGHT_POINT) {
        vec3 toLight = lighting.lights[lightIndex].position.xyz - fragPos;
        float distance = length(toLight);
        lightDir = normalize(toLight);
        attenuation = calculateAttenuation(distance, range);
    }
    else if (lightType == LIGHT_SPOT) {
        vec3 toLight = lighting.lights[lightIndex].position.xyz - fragPos;
        float distance = length(toLight);
        lightDir = normalize(toLight);
        attenuation = calculateAttenuation(distance, range);

        // Spot cone attenuation - access UBO directly
        float cosTheta = dot(lightDir, normalize(-lighting.lights[lightIndex].direction.xyz));
        float innerCone = lighting.lights[lightIndex].params.x;
        float outerCone = lighting.lights[lightIndex].params.y;
        float spotFactor = clamp((cosTheta - outerCone) / (innerCone - outerCone), 0.0, 1.0);
        attenuation *= spotFactor;
    }

    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = diff * baseColor * lightColor;

    // Specular (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), shininess);
    vec3 specular = spec * lightColor * 0.3;

    // Apply shadow to diffuse and specular (not ambient)
    return (diffuse + specular) * intensity * attenuation * (1.0 - shadow);
}

#endif // OHAO_LIGHTING_BLINN_PHONG_GLSL
