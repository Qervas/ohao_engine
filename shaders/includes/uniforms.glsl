#ifndef UNIFORMS_GLSL
#define UNIFORMS_GLSL

layout(binding = 0) uniform GlobalUniformBuffer {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 viewPos;
    float padding1;

    vec3 lightPos;
    float padding2;
    vec3 lightColor;
    float lightIntensity;

    vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    float padding3;
    float padding4;
} ubo;

#endif
