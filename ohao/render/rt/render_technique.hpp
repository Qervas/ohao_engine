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
#include <span>
#include <string>
#include <cstdint>

namespace ohao::render { struct LightDesc; }

namespace ohao {

class Scene;
class Camera;

// The hybrid frame's quality (sub-project 2b spec, "Reference quality"). Realtime is the
// budgeted frame; reference traces every shadowed light with 8 rays a frame and averages
// the frames since the last temporal reset, the realtime frame's converged answer.
enum class RenderQuality : uint8_t { Realtime, Reference };

// ─── Shadow Technique ────────────────────────────────────────────────

struct ShadowInput {
    VkImageView positionBuffer;    // world-space position (GBuffer0)
    VkImageView normalBuffer;      // world-space normal (GBuffer1)
    VkImageView depthBuffer;       // depth buffer
    VkImageView velocityBuffer = VK_NULL_HANDLE;   // UV-space motion (GBuffer3): the filter's reprojection
    uint32_t width;
    uint32_t height;
    uint32_t frameSlot = 0;        // frame-in-flight slot; selects this frame's descriptor set
    uint64_t frameNumber = 0;      // the renderer's frame number (render.stats' shadowLightsDropped)
    glm::vec3 cameraPos{0.0f};

    // The scene's shadow-casting lights (RenderScene::shadowLights, or the fallback sun),
    // <= kShadowMaxLights: light i is visibility channel i (the light UBO's params.w).
    std::span<const render::LightDesc> lights;
    // Sample seeding: the render seed and the frame index since the last temporal reset.
    uint32_t renderSeed = 0;
    uint32_t frameIndex = 0;

    RenderQuality quality = RenderQuality::Realtime;
    uint32_t maxLightsPerPixel = 4;   // realtime: the lights a pixel traces at most (0-4)

    // RT-specific: the RenderScene's TLAS (null if rasterization-only)
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
};

struct ShadowOutput {
    // Visibility, one 8-bit channel a shadow light (0 shadowed, 255 lit), in an RG32UI image:
    // x holds lights 0-3, y lights 4-7, the lowest byte first.
    VkImageView visibility;
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
    uint32_t frameIndex = 0;       // since the last temporal reset: seeds samples; 0 takes no history
    uint32_t renderSeed = 0;       // the render seed: with frameIndex, pixel and sample, seeds each ray
    uint32_t frameSlot = 0;        // frame-in-flight slot; selects this frame's descriptor set

    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;   // RenderScene TLAS (custom index = slot)
    VkBuffer instances = VK_NULL_HANDLE;                // RenderScene instance table (InstanceGpu[])

    // The bounce light: the scene's brightest light (no scene light: intensity 0, no bounce)
    glm::vec3 lightPos{0.0f};
    float lightIntensity{0.0f};
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

    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
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

    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
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
