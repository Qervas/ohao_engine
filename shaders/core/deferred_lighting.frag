#version 450

// Deferred Lighting Fragment Shader
// Reads G-Buffer and evaluates all lights in a single pass.
// CSM: selects cascade based on view-space depth, samples shadow map array.

layout(location = 0) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

// G-Buffer samplers (shader uses bindings 0-2; C++ has 5 gbuffer bindings but 3,4 unused here)
layout(set = 0, binding = 0) uniform sampler2D gBuffer0; // Position + Metallic
layout(set = 0, binding = 1) uniform sampler2D gBuffer1; // Normal + Roughness
layout(set = 0, binding = 2) uniform sampler2D gBuffer2; // Albedo + AO

// Light data — declared as uniform (C++ binds SSBO; NVIDIA is permissive)
struct Light {
    vec4 position;      // xyz = position, w = type (0=dir, 1=point, 2=spot)
    vec4 direction;     // xyz = direction, w = range
    vec4 color;         // xyz = color, w = intensity
    vec4 params;        // x = innerCone, y = outerCone, z = shadowMapIndex, w = unused
    mat4 lightSpaceMatrix;
};

#define MAX_LIGHTS 8  // Must match C++ MAX_LIGHTS in offscreen_renderer.hpp

layout(set = 0, binding = 5) uniform LightingUBO {
    Light lights[MAX_LIGHTS];
    int numLights;
    float ambientIntensity;
    float shadowBias;
    float shadowStrength;
} lighting;

// Shadow map — CSM array (4 cascades)
layout(set = 0, binding = 6) uniform sampler2DArray shadowMap;

// SSGI texture (binding 11) — half-res indirect lighting
layout(set = 0, binding = 11) uniform sampler2D ssgiTexture;

// Cascade shadow map data (binding 12)
layout(set = 0, binding = 12) uniform CascadeUBO {
    mat4 viewProj[4];
    vec4 splitDepths;       // positive view-space distances per cascade
    float cascadeBlendWidth;
    float shadowBias;
    float normalBias;
    uint  cascadeCount;
} cascades;

// Push constants (matches C++ LightingParams — 184 bytes, under NVIDIA 256-byte limit)
layout(push_constant) uniform PushConstants {
    mat4 invViewProj;
    mat4 view;           // for cascade depth selection
    vec3 cameraPos;
    float padding1;
    vec2 screenSize;
    uint lightCount;
    uint flags;          // Bit 0: IBL, Bit 1: SSAO, Bit 2: shadows, Bit 3: SSGI
    float wetness;       // 0=dry, 1=fully wet — drives surface material modulation
    float paddingW;
    float snowCover;     // 0=bare, 1=fully snow-covered — white matte coating on flat faces
    float paddingS;
    float frostCover;    // 0=bare, 1=fully frost/ice coated
    float paddingF;
} pc;

// Constants
const float PI = 3.14159265359;
const float EPSILON = 0.0001;

// Normal decoding from octahedron
vec3 decodeNormalOctahedron(vec2 encoded) {
    vec2 f = encoded * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// GGX/Trowbridge-Reitz Normal Distribution
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return nom / max(denom, EPSILON);
}

// Geometry function (Smith's method with GGX)
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return nom / max(denom, EPSILON);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// Fresnel-Schlick
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Calculate attenuation for point/spot lights
float calculateAttenuation(vec3 lightPos, vec3 fragPos, float range) {
    float distance = length(lightPos - fragPos);
    float attenuation = 1.0 / (1.0 + distance * distance / (range * range));
    float falloff = clamp(1.0 - (distance / range), 0.0, 1.0);
    return attenuation * falloff * falloff;
}

// Spot light cone attenuation
float calculateSpotAttenuation(vec3 L, vec3 spotDir, float innerCone, float outerCone) {
    float theta = dot(L, -spotDir);
    return clamp((theta - outerCone) / (innerCone - outerCone), 0.0, 1.0);
}

// Select CSM cascade based on positive view-space depth
int selectCascade(float viewDepth) {
    uint count = min(cascades.cascadeCount, 4u);
    for (int i = 0; i < int(count); ++i) {
        if (viewDepth < cascades.splitDepths[i]) return i;
    }
    return int(count) - 1;
}

// PCF shadow calculation using CSM array
float calculateShadowCSM(vec3 fragPos, float viewDepth) {
    int cascadeIdx = selectCascade(viewDepth);
    mat4 lightSpaceMatrix = cascades.viewProj[cascadeIdx];

    vec4 fragPosLightSpace = lightSpaceMatrix * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    // NDC [-1,1] -> UV [0,1] for X and Y
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Discard fragments outside the light frustum
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 1.0;
    }

    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float bias = cascades.shadowBias;

    // 3x3 PCF
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap,
                vec3(projCoords.xy + vec2(x, y) * texelSize, float(cascadeIdx))).r;
            shadow += (projCoords.z - bias > pcfDepth) ? 0.0 : 1.0;
        }
    }

    return shadow / 9.0;
}

void main() {
    // Sample G-Buffer
    vec4 gBuffer0Sample = texture(gBuffer0, inTexCoord);
    vec4 gBuffer1Sample = texture(gBuffer1, inTexCoord);
    vec4 gBuffer2Sample = texture(gBuffer2, inTexCoord);

    // Early discard for sky/background — position (0,0,0) means no geometry was written
    if (gBuffer0Sample.rgb == vec3(0.0) && gBuffer0Sample.a == 0.0) {
        outColor = vec4(0.0);
        return;
    }

    // Unpack G-Buffer
    vec3 fragPos = gBuffer0Sample.rgb;
    float metallic = gBuffer0Sample.a;

    vec3 N = decodeNormalOctahedron(gBuffer1Sample.xy);
    float roughness = gBuffer1Sample.a;

    vec3 albedo = gBuffer2Sample.rgb;
    float ao = gBuffer2Sample.a;

    // Ground wetness — modulate material based on rain accumulation.
    // Slope factor: horizontal faces (N.y near 1) accumulate water; vertical walls don't.
    if (pc.wetness > 0.001) {
        float slopeFactor = clamp(N.y * 2.0, 0.0, 1.0);
        float w = pc.wetness * slopeFactor;
        albedo    *= 1.0 - w * 0.30;
        roughness  = mix(roughness, max(roughness * 0.15, 0.02), w);
    }

    // Snow accumulation — white matte coating on upward-facing surfaces.
    // Steeper cutoff than wetness: snow only sticks to surfaces above ~17° from horizontal.
    if (pc.snowCover > 0.001) {
        float slopeFactor = clamp((N.y - 0.3) * (1.0 / 0.7), 0.0, 1.0);
        float s = pc.snowCover * slopeFactor;
        albedo    = mix(albedo,    vec3(0.82, 0.87, 0.92), s); // white-ish snow tint
        roughness = mix(roughness, 0.90,                    s); // snow is matte
        metallic  = mix(metallic,  0.0,                     s); // snow is non-metallic
    }

    // Mud — high wetness on non-metallic surfaces develops an earth-tone tint.
    // Only appears at wetness > 0.5 to avoid tinting lightly-wet surfaces.
    if (pc.wetness > 0.5) {
        float slopeFactor = clamp(N.y * 2.0, 0.0, 1.0);
        float w = pc.wetness * slopeFactor;
        float mud = clamp((w - 0.5) * 2.0, 0.0, 1.0) * (1.0 - metallic);
        const vec3 mudTint = vec3(0.35, 0.27, 0.18);  // dark earth
        albedo = mix(albedo, albedo * mudTint, mud * 0.55);
    }

    // Frost / Ice — forms when snow accumulation is high; crystalline coating.
    if (pc.frostCover > 0.001) {
        float f = pc.frostCover;
        albedo    = mix(albedo,    vec3(0.78, 0.88, 0.95), f * 0.50);  // pale ice blue
        roughness = mix(roughness, 0.08,                    f * 0.70);  // ice = very smooth
        metallic  = mix(metallic,  0.10,                    f * 0.40);  // slight reflectance
    }

    // View direction — use push constant camera position
    vec3 V = normalize(pc.cameraPos - fragPos);

    // View-space depth for cascade selection (positive distance from camera)
    vec4 fragPosView = pc.view * vec4(fragPos, 1.0);
    float viewDepth = -fragPosView.z; // negate: right-handed view space, z is negative in front

    // Calculate F0 (base reflectivity)
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // Accumulate lighting from SSBO data
    vec3 Lo = vec3(0.0);

    // Use light count from push constants (avoids SSBO layout issues with numLights offset)
    int lightCount = int(min(pc.lightCount, uint(MAX_LIGHTS)));
    for (int i = 0; i < lightCount; ++i) {
        Light light = lighting.lights[i];
        int lightType = int(light.position.w);

        vec3 L;
        float attenuation = 1.0;

        if (lightType == 0) {
            L = normalize(-light.direction.xyz);
        } else if (lightType == 1) {
            vec3 lightPos = light.position.xyz;
            L = normalize(lightPos - fragPos);
            attenuation = calculateAttenuation(lightPos, fragPos, light.direction.w);
        } else {
            vec3 lightPos = light.position.xyz;
            L = normalize(lightPos - fragPos);
            attenuation = calculateAttenuation(lightPos, fragPos, light.direction.w);
            attenuation *= calculateSpotAttenuation(L, normalize(light.direction.xyz),
                                                     light.params.x, light.params.y);
        }

        vec3 H = normalize(V + L);

        // Cook-Torrance BRDF
        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + EPSILON;
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;

        float NdotL = max(dot(N, L), 0.0);
        vec3 radiance = light.color.rgb * light.color.w * attenuation;

        // Cascaded shadow mapping for directional lights
        float shadow = 1.0;
        if ((pc.flags & 4u) != 0u && lightType == 0) {
            shadow = calculateShadowCSM(fragPos, viewDepth);
        }

        Lo += (kD * albedo / PI + specular) * radiance * NdotL * shadow;
    }

    // Ambient lighting
    vec3 ambient = vec3(lighting.ambientIntensity) * albedo * ao;

    // Add SSGI indirect lighting when enabled (flag bit 3)
    if ((pc.flags & 8u) != 0u) {
        vec3 ssgiColor = texture(ssgiTexture, inTexCoord).rgb;
        ambient += ssgiColor * albedo * ao;
    }

    // Final color
    vec3 color = ambient + Lo;

    outColor = vec4(color, 1.0);
}
