#pragma once

// Render Technique Interfaces — pluggable rendering methods.
//
// Each feature (shadows, GI, reflections, AO) has an interface.
// Multiple implementations exist per interface:
//   IShadowTechnique  →  CSMShadows, RTShadows
//   IGITechnique      →  SSGI, RTGI, PathTracedGI
//   IReflectionTech   →  SSR, RTReflections
//   IAOTechnique      →  SSAO, RTAO
//
// The renderer holds one active technique per feature.
// Swap at runtime: renderer.setShadowTechnique(std::make_unique<RTShadows>());

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <cstdint>

namespace ohao {

class Scene;
class Camera;
class RTAccelerationStructure;

// ─── Shadow Technique ────────────────────────────────────────────────

struct ShadowInput {
    VkImageView positionBuffer;    // world-space position (GBuffer0)
    VkImageView normalBuffer;      // world-space normal (GBuffer1)
    VkImageView depthBuffer;       // depth buffer
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 cameraPos;
    uint32_t width;
    uint32_t height;

    // Light data
    glm::vec3 lightDirection;      // for directional lights
    glm::vec3 lightPosition;       // for point/spot lights
    int lightType;                 // 0=dir, 1=point, 2=spot
    float lightRange;

    // RT-specific (null if rasterization-only)
    RTAccelerationStructure* accel = nullptr;
};

struct ShadowOutput {
    VkImage shadowMask;            // R8 or R16F — 0=shadowed, 1=lit
    VkImageView shadowMaskView;
};

class IShadowTechnique {
public:
    virtual ~IShadowTechnique() = default;

    virtual const char* getName() const = 0;
    virtual bool needsRT() const = 0;

    virtual bool init(VkDevice device, VkPhysicalDevice physicalDevice,
                      uint32_t width, uint32_t height) = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;
    virtual void render(VkCommandBuffer cmd, const ShadowInput& input) = 0;
    virtual ShadowOutput getOutput() const = 0;
    virtual void destroy() = 0;
};

// ─── Global Illumination Technique ───────────────────────────────────

struct GIInput {
    VkImageView positionBuffer;
    VkImageView normalBuffer;
    VkImageView albedoBuffer;
    VkImageView depthBuffer;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 cameraPos;
    uint32_t width;
    uint32_t height;
    uint32_t frameIndex;           // for temporal accumulation

    RTAccelerationStructure* accel = nullptr;
};

struct GIOutput {
    VkImage indirectLight;         // RGB16F — indirect illumination
    VkImageView indirectLightView;
};

class IGITechnique {
public:
    virtual ~IGITechnique() = default;

    virtual const char* getName() const = 0;
    virtual bool needsRT() const = 0;

    virtual bool init(VkDevice device, VkPhysicalDevice physicalDevice,
                      uint32_t width, uint32_t height) = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;
    virtual void render(VkCommandBuffer cmd, const GIInput& input) = 0;
    virtual GIOutput getOutput() const = 0;
    virtual void destroy() = 0;
};

// ─── Reflection Technique ────────────────────────────────────────────

struct ReflectionInput {
    VkImageView positionBuffer;
    VkImageView normalBuffer;
    VkImageView albedoBuffer;      // for metallic reflections
    VkImageView depthBuffer;
    VkImageView litSceneBuffer;    // current frame lighting (for SSR)
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 cameraPos;
    uint32_t width;
    uint32_t height;
    float roughnessThreshold;      // skip reflections above this roughness

    RTAccelerationStructure* accel = nullptr;
};

struct ReflectionOutput {
    VkImage reflections;           // RGBA16F — reflected color + confidence
    VkImageView reflectionsView;
};

class IReflectionTechnique {
public:
    virtual ~IReflectionTechnique() = default;

    virtual const char* getName() const = 0;
    virtual bool needsRT() const = 0;

    virtual bool init(VkDevice device, VkPhysicalDevice physicalDevice,
                      uint32_t width, uint32_t height) = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;
    virtual void render(VkCommandBuffer cmd, const ReflectionInput& input) = 0;
    virtual ReflectionOutput getOutput() const = 0;
    virtual void destroy() = 0;
};

// ─── AO Technique ────────────────────────────────────────────────────

struct AOInput {
    VkImageView positionBuffer;
    VkImageView normalBuffer;
    VkImageView depthBuffer;
    glm::mat4 view;
    glm::mat4 proj;
    uint32_t width;
    uint32_t height;
    float radius;
    float intensity;

    RTAccelerationStructure* accel = nullptr;
};

struct AOOutput {
    VkImage aoMask;                // R8 — 0=occluded, 1=visible
    VkImageView aoMaskView;
};

class IAOTechnique {
public:
    virtual ~IAOTechnique() = default;

    virtual const char* getName() const = 0;
    virtual bool needsRT() const = 0;

    virtual bool init(VkDevice device, VkPhysicalDevice physicalDevice,
                      uint32_t width, uint32_t height) = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;
    virtual void render(VkCommandBuffer cmd, const AOInput& input) = 0;
    virtual AOOutput getOutput() const = 0;
    virtual void destroy() = 0;
};

} // namespace ohao
