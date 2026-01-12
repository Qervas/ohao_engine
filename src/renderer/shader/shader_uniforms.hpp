#pragma once
#include <glm/glm.hpp>
#include "renderer/lighting/unified_light.hpp"

namespace ohao {

// This struct must match the layout in shaders/includes/uniforms.glsl EXACTLY
// CRITICAL: Use UnifiedLight from unified_light.hpp for consistency

struct GlobalUniformBuffer {
    // Camera matrices (192 bytes)
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 viewPos;
    float padding1;

    // Legacy single light - for compatibility (32 bytes)
    glm::vec3 lightPos;
    float padding2;
    glm::vec3 lightColor;
    float lightIntensity;

    // Material properties - passed via push constants, kept for compatibility (32 bytes)
    glm::vec3 baseColor;
    float metallic;
    float roughness;
    float ao;
    float padding3;
    float padding4;

    // Unified lighting system (1024 + 16 = 1040 bytes)
    UnifiedLight lights[MAX_UNIFIED_LIGHTS];  // 8 * 128 = 1024 bytes
    int numLights;                             // 4 bytes
    float shadowBias;                          // 4 bytes
    float shadowStrength;                      // 4 bytes
    float padding5;                            // 4 bytes

    // Legacy: Single light space matrix for backward compatibility (64 bytes)
    glm::mat4 lightSpaceMatrix;
};

// Note: The actual size depends on std140 alignment rules
// The struct size should be verified against the GLSL layout at runtime if needed

// Legacy RenderLight structure - kept for backward compatibility during migration
// New code should use UnifiedLight instead
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

// Helper to convert old RenderLight to UnifiedLight (for migration)
inline UnifiedLight convertRenderLightToUnified(
    const glm::vec3& position,
    float type,
    const glm::vec3& color,
    float intensity,
    const glm::vec3& direction,
    float range,
    float innerCone = 0.0f,
    float outerCone = 0.0f
) {
    UnifiedLight light{};
    light.position = position;
    light.type = type;
    light.color = color;
    light.intensity = intensity;
    light.direction = direction;
    light.range = range;
    light.innerCone = innerCone;
    light.outerCone = outerCone;
    light.shadowMapIndex = -1;  // No shadow by default
    light.lightSpaceMatrix = glm::mat4(1.0f);
    return light;
}

// Separate UBO for shadow pass (simpler, only needs light space matrix)
struct ShadowUniformBuffer {
    glm::mat4 lightSpaceMatrix;
};

} // namespace ohao
