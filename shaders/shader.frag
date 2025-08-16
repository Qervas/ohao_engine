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

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// Blinn-Phong lighting calculation
vec3 calculateBlinnPhong(vec3 lightPos, vec3 lightColor, float lightIntensity, 
                         vec3 fragPos, vec3 normal, vec3 viewDir, 
                         vec3 baseColor, float metallic, float roughness) {
    vec3 lightDir = normalize(lightPos - fragPos);
    float distance = length(lightPos - fragPos);
    float attenuation = max(lightIntensity / (distance * distance), 0.01);
    
    // Diffuse component
    float NdotL = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = baseColor * NdotL;
    
    // Specular component (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(normal, halfwayDir), 0.0);
    
    // Convert roughness to shininess (inverse relationship)
    float shininess = mix(128.0, 8.0, roughness);
    float spec = pow(NdotH, shininess);
    
    // Fresnel approximation for metals
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    vec3 F = F0 + (1.0 - F0) * pow(clamp(1.0 - max(dot(halfwayDir, viewDir), 0.0), 0.0, 1.0), 5.0);
    
    vec3 specular = F * spec;
    
    return (diffuse + specular) * lightColor * attenuation;
}

// Point light calculation
vec3 calculatePointLight(RenderLight light, vec3 fragPos, vec3 normal, vec3 viewDir,
                         vec3 baseColor, float metallic, float roughness) {
    vec3 lightDir = normalize(light.position - fragPos);
    float distance = length(light.position - fragPos);
    
    // Attenuation with range
    float attenuation = 1.0 / (1.0 + 0.09 * distance + 0.032 * distance * distance);
    if (distance > light.range) attenuation = 0.0;
    
    return calculateBlinnPhong(light.position, light.color, light.intensity * attenuation,
                               fragPos, normal, viewDir, baseColor, metallic, roughness);
}

// Directional light calculation
vec3 calculateDirectionalLight(RenderLight light, vec3 normal, vec3 viewDir,
                               vec3 baseColor, float metallic, float roughness) {
    vec3 lightDir = normalize(-light.direction);
    
    // Diffuse component
    float NdotL = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = baseColor * NdotL;
    
    // Specular component (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float NdotH = max(dot(normal, halfwayDir), 0.0);
    
    float shininess = mix(128.0, 8.0, roughness);
    float spec = pow(NdotH, shininess);
    
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    vec3 F = F0 + (1.0 - F0) * pow(clamp(1.0 - max(dot(halfwayDir, viewDir), 0.0), 0.0, 1.0), 5.0);
    
    vec3 specular = F * spec;
    
    return (diffuse + specular) * light.color * light.intensity;
}

// Spot light calculation
vec3 calculateSpotLight(RenderLight light, vec3 fragPos, vec3 normal, vec3 viewDir,
                        vec3 baseColor, float metallic, float roughness) {
    vec3 lightDir = normalize(light.position - fragPos);
    float distance = length(light.position - fragPos);
    
    // Basic attenuation with range
    float attenuation = 1.0 / (1.0 + 0.09 * distance + 0.032 * distance * distance);
    if (distance > light.range) attenuation = 0.0;
    
    // Spot light cone calculation
    vec3 spotDir = normalize(light.direction);
    float theta = dot(lightDir, -spotDir); // Angle between light direction and direction to fragment
    
    // Convert angles from degrees to cosine values for comparison
    float innerCutoff = cos(radians(light.innerCone));
    float outerCutoff = cos(radians(light.outerCone));
    
    // Smooth falloff between inner and outer cone
    float epsilon = innerCutoff - outerCutoff;
    float intensity = clamp((theta - outerCutoff) / epsilon, 0.0, 1.0);
    
    // Apply spotlight intensity to attenuation
    attenuation *= intensity;
    
    return calculateBlinnPhong(light.position, light.color, light.intensity * attenuation,
                               fragPos, normal, viewDir, baseColor, metallic, roughness);
}

void main() {
    // Add small depth offset to prevent z-fighting
    gl_FragDepth = gl_FragCoord.z - 0.0001;

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(ubo.viewPos - fragPos);

    // Double-sided lighting: flip normal if needed
    if (dot(N, V) < 0.0) {
        N = -N;
    }

    // Use material properties from push constants
    vec3 materialColor = fragBaseColor;
    float metallic = fragMetallic;
    float roughness = fragRoughness;
    float ao = fragAo;

    // Initialize lighting with ambient
    vec3 ambient = 0.2 * materialColor * ao;
    vec3 lighting = ambient;

    // Process multiple lights
    for (int i = 0; i < min(ubo.numLights, MAX_LIGHTS); ++i) {
        RenderLight light = ubo.lights[i];
        
        if (light.type == 0.0) { // Directional light
            lighting += calculateDirectionalLight(light, N, V, materialColor, metallic, roughness);
        }
        else if (light.type == 1.0) { // Point light
            lighting += calculatePointLight(light, fragPos, N, V, materialColor, metallic, roughness);
        }
        else if (light.type == 2.0) { // Spot light
            lighting += calculateSpotLight(light, fragPos, N, V, materialColor, metallic, roughness);
        }
    }
    
    // Fallback to legacy single light if no lights are defined
    if (ubo.numLights == 0) {
        lighting += calculateBlinnPhong(ubo.lightPos, ubo.lightColor, ubo.lightIntensity,
                                       fragPos, N, V, materialColor, metallic, roughness);
    }

    // HDR tone mapping
    vec3 color = lighting / (lighting + vec3(1.0));

    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
