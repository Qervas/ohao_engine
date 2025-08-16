#pragma once
#include <glm/glm.hpp>

namespace ohao {

#define MAX_LIGHTS 8

struct RenderLight {
    glm::vec3 position;
    float type; // 0=directional, 1=point, 2=spot
    
    glm::vec3 color;
    float intensity;
    
    glm::vec3 direction; // for directional/spot lights
    float range;         // for point/spot lights
    
    float innerCone; // for spot lights
    float outerCone; // for spot lights
    glm::vec2 padding;
};

// This struct must match the layout in shaders
struct GlobalUniformBuffer {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 viewPos;
    float padding1;

    // Legacy single light (for compatibility)
    glm::vec3 lightPos;
    float padding2;
    glm::vec3 lightColor;
    float lightIntensity;

    // Material properties (now unused - passed via push constants)
    glm::vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    float padding3;
    float padding4;
    
    // Multiple lights support
    RenderLight lights[MAX_LIGHTS];
    int numLights;
    glm::vec3 padding5;
};

} // namespace ohao
