#pragma once
#include <glm/glm.hpp>

namespace ohao {

// This struct must match the layout in shaders
struct GlobalUniformBuffer {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 viewPos;
    float padding1;

    glm::vec3 lightPos;
    float padding2;
    glm::vec3 lightColor;
    float lightIntensity;

    glm::vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    float padding3;
    float padding4;
};

} // namespace ohao
