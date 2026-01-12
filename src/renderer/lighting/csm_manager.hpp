#pragma once
/**
 * csm_manager.hpp - Cascaded Shadow Map Manager
 *
 * Implements cascaded shadow maps for directional light shadows with:
 * - 4 cascades at configurable resolution (default 2048x2048)
 * - Practical split scheme (logarithmic + linear blend)
 * - Cascade stabilization to prevent shadow edge shimmer
 * - Type-safe cascade access via CascadeIndex handle
 *
 * COMPILE-TIME SAFETY:
 * - Uses constants from ShaderBindings for array sizes
 * - static_assert validates UBO layout matches GPU expectations
 * - CascadeIndex handle prevents array bounds errors
 */

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <optional>
#include "renderer/shader/shader_bindings.hpp"
#include "renderer/lighting/unified_light.hpp"

namespace ohao {

class VulkanContext;
class OhaoVkImage;
class OhaoVkUniformBuffer;

// =============================================================================
// CSM CONFIGURATION
// =============================================================================

/**
 * @brief Configuration for cascaded shadow map system
 *
 * All defaults come from ShaderBindings for consistency.
 */
struct CSMConfig {
    /// Resolution of each cascade shadow map
    uint32_t cascadeResolution = ShaderBindings::CSM::kCascadeResolution;

    /// Split lambda (0 = uniform, 1 = logarithmic, 0.5 = blend)
    float splitLambda = ShaderBindings::CSM::kDefaultSplitLambda;

    /// Maximum shadow distance from camera
    float shadowDistance = ShaderBindings::CSM::kDefaultShadowDistance;

    /// Near clip plane for shadow projection
    float nearClip = ShaderBindings::CSM::kDefaultNearClip;

    /// Far clip plane for shadow projection
    float farClip = ShaderBindings::CSM::kDefaultFarClip;

    /// Shadow bias to prevent acne
    float shadowBias = 0.005f;

    /// Normal bias for slope-dependent shadow offset
    float normalBias = 0.05f;

    /// Enable cascade stabilization (prevents shimmer)
    bool stabilize = true;
};

// =============================================================================
// CASCADE DATA
// =============================================================================

/**
 * @brief Per-cascade computed data
 *
 * This structure is internal to the CPU-side calculation.
 * The GPU sees CSMCascadeInfo from unified_light.hpp.
 */
struct CascadeData {
    glm::mat4 viewProj{1.0f};        ///< Light-space view-projection matrix
    float splitNear{0.0f};            ///< Near split depth (view space)
    float splitFar{0.0f};             ///< Far split depth (view space)
    float texelSize{0.0f};            ///< Shadow map texel size for filtering
    glm::vec4 frustumCorners[8];      ///< Frustum corners for this cascade
};

// =============================================================================
// CSM MANAGER
// =============================================================================

/**
 * @brief Manages cascaded shadow maps for directional light
 *
 * The CSM manager owns:
 * - 4 depth textures for cascade shadow maps
 * - A render pass for depth-only rendering
 * - Per-cascade framebuffers
 * - UBO for cascade data (split depths, view-proj matrices)
 *
 * Usage:
 *   1. Call update() each frame with camera and light data
 *   2. For each cascade, call beginCascade(), render scene, endCascade()
 *   3. Bind cascade textures and UBO for main pass sampling
 */
class CSMManager {
public:
    /// Number of cascades (compile-time constant from ShaderBindings)
    static constexpr uint32_t kNumCascades = ShaderBindings::kMaxCSMCascades;

    CSMManager() = default;
    ~CSMManager();

    // Prevent copying
    CSMManager(const CSMManager&) = delete;
    CSMManager& operator=(const CSMManager&) = delete;

    /**
     * @brief Initialize CSM system with given configuration
     * @param ctx Vulkan context
     * @param config CSM configuration (uses defaults if not specified)
     * @return true on success
     */
    bool initialize(VulkanContext* ctx, const CSMConfig& config = CSMConfig{});

    /**
     * @brief Clean up all Vulkan resources
     */
    void cleanup();

    /**
     * @brief Update cascade splits and matrices
     *
     * Call this each frame before rendering shadow maps.
     *
     * @param cameraView Camera view matrix
     * @param cameraProj Camera projection matrix
     * @param lightDir World-space light direction (normalized)
     * @param cameraNear Camera near plane
     * @param cameraFar Camera far plane (or shadow distance)
     */
    void update(
        const glm::mat4& cameraView,
        const glm::mat4& cameraProj,
        const glm::vec3& lightDir,
        float cameraNear,
        float cameraFar
    );

    /**
     * @brief Begin rendering to a specific cascade
     * @param cmd Command buffer
     * @param cascadeIndex Which cascade to render (0-3)
     */
    void beginCascade(VkCommandBuffer cmd, CascadeIndex cascadeIndex);

    /**
     * @brief End rendering to current cascade
     * @param cmd Command buffer
     */
    void endCascade(VkCommandBuffer cmd);

    /**
     * @brief Update CSM UBO with current cascade data
     * @param frameIndex Current frame index (for multi-buffering)
     */
    void updateUBO(uint32_t frameIndex);

    // =========================================================================
    // Getters
    // =========================================================================

    /**
     * @brief Get cascade data by index
     * @param index Cascade index (bounds-checked)
     * @return Cascade data or nullopt if invalid
     */
    [[nodiscard]] std::optional<CascadeData> getCascadeData(CascadeIndex index) const;

    /**
     * @brief Get light-space matrix for a cascade
     * @param index Cascade index
     * @return View-projection matrix or identity if invalid
     */
    [[nodiscard]] glm::mat4 getLightSpaceMatrix(CascadeIndex index) const;

    /**
     * @brief Get split depth for a cascade (view space)
     * @param index Cascade index
     * @return Split depth or 0 if invalid
     */
    [[nodiscard]] float getSplitDepth(CascadeIndex index) const;

    /**
     * @brief Get depth image view for a cascade
     * @param index Cascade index
     * @return VkImageView or VK_NULL_HANDLE if invalid
     */
    [[nodiscard]] VkImageView getCascadeImageView(CascadeIndex index) const;

    /**
     * @brief Get all cascade image views for descriptor binding
     * @return Array of image views
     */
    [[nodiscard]] std::array<VkImageView, kNumCascades> getCascadeImageViews() const;

    /**
     * @brief Get shadow sampler (shared for all cascades)
     */
    [[nodiscard]] VkSampler getShadowSampler() const { return shadowSampler; }

    /**
     * @brief Get render pass for shadow rendering
     */
    [[nodiscard]] VkRenderPass getRenderPass() const { return renderPass; }

    /**
     * @brief Get cascade resolution
     */
    [[nodiscard]] uint32_t getCascadeResolution() const { return config.cascadeResolution; }

    /**
     * @brief Check if CSM system is ready
     */
    [[nodiscard]] bool isInitialized() const { return initialized; }

    /**
     * @brief Get configuration
     */
    [[nodiscard]] const CSMConfig& getConfig() const { return config; }

    /**
     * @brief Modify configuration (requires re-initialization)
     */
    CSMConfig& getConfigMutable() { return config; }

private:
    VulkanContext* context{nullptr};
    CSMConfig config{};
    bool initialized{false};

    // Per-cascade resources
    std::array<std::unique_ptr<OhaoVkImage>, kNumCascades> cascadeDepthImages;
    std::array<VkFramebuffer, kNumCascades> cascadeFramebuffers{};
    std::array<CascadeData, kNumCascades> cascadeData{};

    // Shared resources
    VkRenderPass renderPass{VK_NULL_HANDLE};
    VkSampler shadowSampler{VK_NULL_HANDLE};
    std::unique_ptr<OhaoVkUniformBuffer> csmUBO;

    // Current state
    int32_t currentCascade{-1};

    // =========================================================================
    // Internal methods
    // =========================================================================

    bool createDepthImages();
    bool createRenderPass();
    bool createFramebuffers();
    bool createShadowSampler();
    bool createUBO();

    /**
     * @brief Calculate cascade split depths using practical split scheme
     *
     * Uses blend of logarithmic and uniform distribution:
     * split[i] = lambda * log_split + (1 - lambda) * uniform_split
     *
     * @param nearClip Near plane distance
     * @param farClip Far plane distance (or shadow distance)
     * @param splits Output array of split depths
     */
    void calculateSplitDepths(float nearClip, float farClip, std::array<float, kNumCascades + 1>& splits) const;

    /**
     * @brief Calculate light-space matrix for a cascade
     *
     * Creates a tight-fitting orthographic projection around the cascade frustum.
     * Optionally stabilizes the projection to prevent shadow edge shimmer.
     *
     * @param cascadeIdx Which cascade (0-3)
     * @param lightDir Light direction
     * @param frustumCorners Frustum corners for this cascade
     * @param stabilize Whether to stabilize the projection
     * @return Light-space view-projection matrix
     */
    glm::mat4 calculateCascadeMatrix(
        uint32_t cascadeIdx,
        const glm::vec3& lightDir,
        const glm::vec4 frustumCorners[8],
        bool stabilize
    ) const;

    /**
     * @brief Get frustum corners for a cascade in world space
     *
     * @param invViewProj Inverse of camera view-projection
     * @param splitNear Near split distance (0-1 in NDC)
     * @param splitFar Far split distance (0-1 in NDC)
     * @param outCorners Output array of 8 corners
     */
    void getFrustumCornersWorldSpace(
        const glm::mat4& invViewProj,
        float splitNear,
        float splitFar,
        glm::vec4 outCorners[8]
    ) const;
};

// =============================================================================
// COMPILE-TIME VALIDATION
// =============================================================================

static_assert(CSMManager::kNumCascades == 4,
              "CSM system designed for 4 cascades - update shaders if changed");

static_assert(CSMManager::kNumCascades == ShaderBindings::kMaxCSMCascades,
              "CSMManager cascade count must match ShaderBindings");

} // namespace ohao
