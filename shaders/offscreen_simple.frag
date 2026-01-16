#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragPos;

layout(location = 0) out vec4 outColor;

// Light types
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2

// Maximum lights (must match C++ MAX_LIGHTS)
#define MAX_LIGHTS 8

// Light data structure (matches C++ LightData)
struct Light {
    vec4 position;      // xyz = position, w = type
    vec4 direction;     // xyz = direction, w = range
    vec4 color;         // xyz = color, w = intensity
    vec4 params;        // x = innerCone, y = outerCone
};

// Camera uniform (binding 0)
layout(binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 viewPos;
} camera;

// Light uniform (binding 1)
layout(binding = 1) uniform LightUBO {
    Light lights[MAX_LIGHTS];
    int numLights;
    float ambientIntensity;
    vec2 padding;
} lighting;

// Calculate attenuation for point/spot lights
float calculateAttenuation(float distance, float range) {
    // Quadratic falloff with range
    float attenuation = 1.0 / (1.0 + (distance * distance) / (range * range));
    // Smooth edge falloff
    float edgeFactor = clamp(1.0 - distance / range, 0.0, 1.0);
    return attenuation * edgeFactor * edgeFactor;
}

// Calculate light contribution
vec3 calculateLight(Light light, vec3 normal, vec3 viewDir, vec3 baseColor) {
    int lightType = int(light.position.w);
    vec3 lightColor = light.color.rgb;
    float intensity = light.color.w;
    float range = light.direction.w;

    vec3 lightDir;
    float attenuation = 1.0;

    if (lightType == LIGHT_DIRECTIONAL) {
        lightDir = normalize(-light.direction.xyz);
    }
    else if (lightType == LIGHT_POINT) {
        vec3 toLight = light.position.xyz - fragPos;
        float distance = length(toLight);
        lightDir = normalize(toLight);
        attenuation = calculateAttenuation(distance, range);
    }
    else if (lightType == LIGHT_SPOT) {
        vec3 toLight = light.position.xyz - fragPos;
        float distance = length(toLight);
        lightDir = normalize(toLight);
        attenuation = calculateAttenuation(distance, range);

        // Spot cone attenuation
        float cosTheta = dot(lightDir, normalize(-light.direction.xyz));
        float innerCone = light.params.x;
        float outerCone = light.params.y;
        float spotFactor = clamp((cosTheta - outerCone) / (innerCone - outerCone), 0.0, 1.0);
        attenuation *= spotFactor;
    }

    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = diff * baseColor * lightColor;

    // Specular (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), 32.0);
    vec3 specular = spec * lightColor * 0.3;

    return (diffuse + specular) * intensity * attenuation;
}

void main() {
    vec3 normal = normalize(fragNormal);
    vec3 viewDir = normalize(camera.viewPos - fragPos);

    // Ambient
    vec3 ambient = lighting.ambientIntensity * fragColor;

    // Accumulate light contributions
    vec3 lighting_result = vec3(0.0);
    for (int i = 0; i < lighting.numLights && i < MAX_LIGHTS; i++) {
        lighting_result += calculateLight(lighting.lights[i], normal, viewDir, fragColor);
    }

    vec3 result = ambient + lighting_result;

    // Gamma correction
    result = pow(result, vec3(1.0 / 2.2));

    outColor = vec4(result, 1.0);
}
