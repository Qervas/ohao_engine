#version 450

// Deferred Lighting Fragment Shader
// Reads G-Buffer and evaluates all lights in a single pass

layout(location = 0) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

// G-Buffer samplers
layout(set = 0, binding = 0) uniform sampler2D gBuffer0; // Position + Metallic
layout(set = 0, binding = 1) uniform sampler2D gBuffer1; // Normal + Roughness
layout(set = 0, binding = 2) uniform sampler2D gBuffer2; // Albedo + AO
layout(set = 0, binding = 3) uniform sampler2D gDepth;   // Depth buffer

// Shadow map
layout(set = 0, binding = 4) uniform sampler2D shadowMap;

// Light data (matches C++ struct)
struct Light {
    vec4 position;      // xyz = position, w = type (0=dir, 1=point, 2=spot)
    vec4 direction;     // xyz = direction, w = range
    vec4 color;         // xyz = color, w = intensity
    vec4 params;        // x = innerCone, y = outerCone, z = shadowMapIndex, w = unused
    mat4 lightSpaceMatrix;
};

#define MAX_LIGHTS 256  // Increased limit for deferred rendering

layout(set = 0, binding = 5) uniform LightingUBO {
    Light lights[MAX_LIGHTS];
    int numLights;
    float ambientIntensity;
    float shadowBias;
    float shadowStrength;
    vec3 viewPos;
} lighting;

// Constants
const float PI = 3.14159265359;
const float EPSILON = 0.0001;

// Normal decoding from octahedron
vec3 decodeNormalOctahedron(vec2 encoded) {
    // Map from [0,1] to [-1,1]
    vec2 f = encoded * 2.0 - 1.0;

    // Reconstruct Z
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));

    // Handle bottom hemisphere
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

    // Smooth falloff at range
    float falloff = clamp(1.0 - (distance / range), 0.0, 1.0);
    return attenuation * falloff * falloff;
}

// Spot light cone attenuation
float calculateSpotAttenuation(vec3 L, vec3 spotDir, float innerCone, float outerCone) {
    float theta = dot(L, -spotDir);
    return clamp((theta - outerCone) / (innerCone - outerCone), 0.0, 1.0);
}

// Shadow calculation
float calculateShadow(vec3 fragPos, mat4 lightSpaceMatrix, float bias) {
    vec4 fragPosLightSpace = lightSpaceMatrix * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;

    // Check if outside shadow map bounds
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 1.0;
    }

    // PCF shadow sampling
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);

    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += (projCoords.z - bias > pcfDepth) ? 0.0 : 1.0;
        }
    }

    return shadow / 25.0;
}

void main() {
    // Sample G-Buffer
    vec4 gBuffer0Sample = texture(gBuffer0, inTexCoord);
    vec4 gBuffer1Sample = texture(gBuffer1, inTexCoord);
    vec4 gBuffer2Sample = texture(gBuffer2, inTexCoord);

    // Early discard for sky/background (check depth or position)
    if (gBuffer0Sample.w == 0.0 && gBuffer2Sample.a == 0.0) {
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

    // View direction
    vec3 V = normalize(lighting.viewPos - fragPos);

    // Calculate F0 (base reflectivity)
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // Accumulate lighting
    vec3 Lo = vec3(0.0);

    for (int i = 0; i < lighting.numLights && i < MAX_LIGHTS; ++i) {
        Light light = lighting.lights[i];
        int lightType = int(light.position.w);

        vec3 L;
        float attenuation = 1.0;

        if (lightType == 0) {
            // Directional light
            L = normalize(-light.direction.xyz);
        } else if (lightType == 1) {
            // Point light
            vec3 lightPos = light.position.xyz;
            L = normalize(lightPos - fragPos);
            attenuation = calculateAttenuation(lightPos, fragPos, light.direction.w);
        } else {
            // Spot light
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

        // kS is F (Fresnel)
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallic;

        float NdotL = max(dot(N, L), 0.0);

        // Light contribution
        vec3 radiance = light.color.rgb * light.color.w * attenuation;

        // Shadow
        float shadow = 1.0;
        if (light.params.z >= 0.0) {
            shadow = mix(1.0, calculateShadow(fragPos, light.lightSpaceMatrix, lighting.shadowBias),
                         lighting.shadowStrength);
        }

        Lo += (kD * albedo / PI + specular) * radiance * NdotL * shadow;
    }

    // Ambient lighting
    vec3 ambient = vec3(lighting.ambientIntensity) * albedo * ao;

    // Final color
    vec3 color = ambient + Lo;

    // Output (HDR - will be tonemapped later)
    outColor = vec4(color, 1.0);
}
