#pragma once
#include <glm/glm.hpp>

namespace ohao {

// GPU-side light data — must match GLSL struct exactly
// 5 vec4s = 80 bytes per light, std430 layout
struct GPULight {
    glm::vec4 positionAndType;    // xyz=position, w=type (0=sphere,1=dir,2=spot,3=area)
    glm::vec4 colorAndIntensity;  // rgb=color, w=intensity
    glm::vec4 dirAndParam;        // xyz=direction, w=param (sphere:radius, spot:innerAngle)
    glm::vec4 extra;              // spot: w=outerAngle, area: xyz=edge1
    glm::vec4 extra2;             // area: xyz=edge2, w=area
};

static constexpr uint32_t MAX_GPU_LIGHTS = 64;

} // namespace ohao
