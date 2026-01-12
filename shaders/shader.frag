#version 450
#include "includes/uniforms.glsl"

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec3 fragColor;
layout(location = 4) in vec3 fragBaseColor;
layout(location = 5) in float fragMetallic;
layout(location = 6) in float fragRoughness;
layout(location = 7) in float fragAo;
layout(location = 8) in vec4 fragPosLightSpace;  // Legacy - kept for compatibility

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// PBR Functions
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// PBR Lighting calculation - uses light index to avoid struct copy issues
vec3 calculatePBRLightByIndex(int lightIndex, vec3 fragPos, vec3 N, vec3 V,
                              vec3 albedo, float metallic, float roughness, float ao) {
    vec3 L, radiance;
    float attenuation = 1.0;

    float lightType = ubo.lights[lightIndex].type;
    vec3 lightPos = ubo.lights[lightIndex].position;
    vec3 lightDir = ubo.lights[lightIndex].direction;
    vec3 lightColor = ubo.lights[lightIndex].color;
    float lightIntensity = ubo.lights[lightIndex].intensity;
    float lightRange = ubo.lights[lightIndex].range;

    if (lightType == LIGHT_TYPE_DIRECTIONAL) {
        L = normalize(-lightDir);
        radiance = lightColor * lightIntensity;
    } else if (lightType == LIGHT_TYPE_POINT) {
        L = normalize(lightPos - fragPos);
        float distance = length(lightPos - fragPos);
        attenuation = 1.0 / (distance * distance);
        if (distance > lightRange) attenuation = 0.0;
        radiance = lightColor * lightIntensity * attenuation;
    } else if (lightType == LIGHT_TYPE_SPOT) {
        L = normalize(lightPos - fragPos);
        float distance = length(lightPos - fragPos);
        attenuation = 1.0 / (distance * distance);
        if (distance > lightRange) attenuation = 0.0;

        // Spot light cone calculation
        vec3 spotDir = normalize(lightDir);
        float theta = dot(L, -spotDir);
        float innerCutoff = cos(radians(ubo.lights[lightIndex].innerCone));
        float outerCutoff = cos(radians(ubo.lights[lightIndex].outerCone));
        float epsilon = innerCutoff - outerCutoff;
        float intensity = clamp((theta - outerCutoff) / epsilon, 0.0, 1.0);
        attenuation *= intensity;

        radiance = lightColor * lightIntensity * attenuation;
    } else {
        return vec3(0.0); // Unknown light type
    }

    vec3 H = normalize(V + L);

    // Calculate F0 (reflectance at normal incidence)
    vec3 F0 = vec3(0.04); // Default for dielectrics
    F0 = mix(F0, albedo, metallic); // Metals use albedo as F0

    // Cook-Torrance BRDF
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;

    // Energy conservation
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic; // Metals don't have diffuse lighting

    float NdotL = max(dot(N, L), 0.0);

    // Lambertian diffuse
    vec3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * radiance * NdotL;
}

// Debug: Set to 1 to visualize shadow map depth, 2 to show shadow coords
#define SHADOW_DEBUG_MODE 0

void main() {
    // Add small depth offset to prevent z-fighting
    gl_FragDepth = gl_FragCoord.z - 0.0001;

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(ubo.viewPos - fragPos);

    // Debug: Visualize shadow map contents
    #if SHADOW_DEBUG_MODE == 1
    vec4 lsPos = ubo.lightSpaceMatrix * vec4(fragPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    if (projCoords.x >= 0.0 && projCoords.x <= 1.0 &&
        projCoords.y >= 0.0 && projCoords.y <= 1.0) {
        float shadowDepth = texture(shadowMaps[0], projCoords.xy).r;
        outColor = vec4(shadowDepth, shadowDepth, shadowDepth, 1.0);
        return;
    }
    #elif SHADOW_DEBUG_MODE == 2
    vec4 lsPos = ubo.lightSpaceMatrix * vec4(fragPos, 1.0);
    vec3 projCoords = lsPos.xyz / lsPos.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    outColor = vec4(projCoords.x, projCoords.y, projCoords.z, 1.0);
    return;
    #endif

    // Double-sided lighting: flip normal if needed
    if (dot(N, V) < 0.0) {
        N = -N;
    }

    // Use material properties from push constants
    vec3 albedo = fragBaseColor;
    float metallic = fragMetallic;
    float roughness = max(fragRoughness, 0.04); // Minimum roughness to prevent artifacts
    float ao = fragAo;

    // Calculate F0 for ambient lighting
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // Initialize lighting with IBL-style ambient
    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    vec3 irradiance = vec3(0.3) * albedo; // Simple ambient approximation
    vec3 diffuse = irradiance * albedo;

    // Simple specular environment approximation
    vec3 specular = vec3(0.1) * F;

    vec3 ambient = (kD * diffuse + specular) * ao;
    vec3 lighting = ambient;

    // Process all lights with unified shadow calculation
    float totalShadow = 0.0;
    for (int i = 0; i < min(ubo.numLights, MAX_LIGHTS); ++i) {
        // Use index-based function to avoid struct copy issues
        vec3 lightContribution = calculatePBRLightByIndex(i, fragPos, N, V, albedo, metallic, roughness, ao);

        // Calculate shadow using the unified system
        float shadow = calculateShadowForLight(i, fragPos, N);
        totalShadow = max(totalShadow, shadow);

        // Apply shadow
        lightContribution *= (1.0 - shadow);

        lighting += lightContribution;
    }

    // HDR tone mapping (ACES approximation)
    vec3 color = lighting;
    color = color / (color + vec3(1.0));

    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
