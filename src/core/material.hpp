#pragma once
#include <glm/glm.hpp>

namespace ohao {

struct Material {
    glm::vec3 baseColor{0.8f, 0.8f, 0.8f};
    float metallic{0.0f}; //0=dielectric, 1=metal
    float roughness{0.5f};
    float ao{1.0f}; //ambient occlusion
    glm::vec3 emissive{0.0f};
    float ior{1.45f}; //index of refraction

};

}
