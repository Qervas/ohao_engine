#pragma once
// A primitive's material for tests: colour, roughness, metalness and emission, everything else
// OpenPBR's defaults (ecs::Material).
#include "ecs/components.hpp"

namespace ohao::test {

inline ecs::Material material(const glm::vec3& color, float roughness, float metalness = 0.0f, float emission = 0.0f) {
    ecs::Material m;
    m.baseColor = color;
    m.specularRoughness = roughness;
    m.baseMetalness = metalness;
    m.emissionLuminance = emission;
    return m;
}

} // namespace ohao::test
