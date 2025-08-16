#ifndef UNIFORMS_GLSL
#define UNIFORMS_GLSL

#define MAX_LIGHTS 8

struct RenderLight {
    vec3 position;
    float type; // 0=directional, 1=point, 2=spot
    
    vec3 color;
    float intensity;
    
    vec3 direction; // for directional/spot lights
    float range;    // for point/spot lights
    
    float innerCone; // for spot lights
    float outerCone; // for spot lights
    vec2 padding;
};

layout(binding = 0) uniform GlobalUniformBuffer {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 viewPos;
    float padding1;

    // Legacy single light (for compatibility)
    vec3 lightPos;
    float padding2;
    vec3 lightColor;
    float lightIntensity;

    // Material properties (now unused - passed via push constants)
    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    float padding3;
    float padding4;
    
    // Multiple lights support
    RenderLight lights[MAX_LIGHTS];
    int numLights;
    vec3 padding5;
} ubo;

#endif
