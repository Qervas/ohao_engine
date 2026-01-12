#pragma once
/**
 * shadow_atlas.hpp - Shadow Atlas for Local Lights
 *
 * Manages a single 4096x4096 depth texture divided into 16 tiles (1024x1024 each)
 * for point and spot light shadow maps.
 *
 * CORE PRINCIPLE: Tile allocation returns std::optional - no runtime surprises
 * when atlas is full.
 *
 * COMPILE-TIME SAFETY:
 * - Uses constants from ShaderBindings for atlas/tile sizes
 * - AtlasTileHandle prevents mixing with other handle types
 * - checkedAccess() for bounds-validated tile lookups
 */

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <bitset>
#include "renderer/shader/shader_bindings.hpp"
#include "renderer/lighting/unified_light.hpp"

namespace ohao {

class VulkanContext;
class OhaoVkImage;

// =============================================================================
// ATLAS ALLOCATION
// =============================================================================

/**
 * @brief Information about an allocated atlas tile
 *
 * Contains UV coordinates for shader sampling and the tile index.
 */
struct AtlasAllocation {
    AtlasTileHandle handle;          ///< Handle to the allocated tile
    glm::vec2 uvOffset;              ///< UV offset into atlas (0-1)
    glm::vec2 uvScale;               ///< UV scale (typically 0.25 for 4x4)
    VkRect2D scissorRect;            ///< Scissor rect for rendering to this tile
    VkViewport viewport;             ///< Viewport for rendering to this tile
};

// =============================================================================
// SHADOW ATLAS
// =============================================================================

/**
 * @brief Manages shadow atlas for local lights (point/spot)
 *
 * The atlas is a single 4096x4096 depth texture divided into a 4x4 grid
 * of 1024x1024 tiles. Each local light that casts shadows gets one tile.
 *
 * Key features:
 * - Single render target (efficient batching)
 * - Tile allocation with std::optional return
 * - Per-tile viewports for scissored rendering
 * - UV offset/scale for shader sampling
 *
 * Usage:
 *   1. Initialize with VulkanContext
 *   2. For each shadow-casting local light, call allocateTile()
 *   3. When light is destroyed, call releaseTile()
 *   4. When rendering, use getViewport()/getScissor() for each tile
 */
class ShadowAtlas {
public:
    /// Atlas size (from ShaderBindings)
    static constexpr uint32_t kAtlasSize = ShaderBindings::ShadowAtlas::kAtlasSize;

    /// Tile size (from ShaderBindings)
    static constexpr uint32_t kTileSize = ShaderBindings::ShadowAtlas::kTileSize;

    /// Tiles per row (from ShaderBindings)
    static constexpr uint32_t kTilesPerRow = ShaderBindings::ShadowAtlas::kTilesPerRow;

    /// Total number of tiles (from ShaderBindings)
    static constexpr uint32_t kTotalTiles = ShaderBindings::ShadowAtlas::kTotalTiles;

    /// UV scale for each tile
    static constexpr float kTileUVScale = ShaderBindings::ShadowAtlas::kTileUVScale;

    ShadowAtlas() = default;
    ~ShadowAtlas();

    // Prevent copying
    ShadowAtlas(const ShadowAtlas&) = delete;
    ShadowAtlas& operator=(const ShadowAtlas&) = delete;

    /**
     * @brief Initialize shadow atlas
     * @param ctx Vulkan context
     * @return true on success
     */
    bool initialize(VulkanContext* ctx);

    /**
     * @brief Clean up all Vulkan resources
     */
    void cleanup();

    // =========================================================================
    // Tile Allocation
    // =========================================================================

    /**
     * @brief Allocate a tile for a light
     *
     * @return AtlasAllocation if successful, std::nullopt if atlas is full
     *
     * IMPORTANT: Returns std::nullopt instead of throwing - caller must handle
     * the case when atlas is full (e.g., skip shadow for this light)
     */
    [[nodiscard]] std::optional<AtlasAllocation> allocateTile();

    /**
     * @brief Release a previously allocated tile
     * @param handle Handle to the tile to release
     */
    void releaseTile(AtlasTileHandle handle);

    /**
     * @brief Check if a tile handle is valid and allocated
     * @param handle Handle to check
     * @return true if tile is allocated
     */
    [[nodiscard]] bool isTileAllocated(AtlasTileHandle handle) const;

    /**
     * @brief Get number of allocated tiles
     */
    [[nodiscard]] uint32_t getAllocatedTileCount() const;

    /**
     * @brief Get number of free tiles
     */
    [[nodiscard]] uint32_t getFreeTileCount() const { return kTotalTiles - getAllocatedTileCount(); }

    // =========================================================================
    // Tile Information
    // =========================================================================

    /**
     * @brief Get UV offset for a tile (for shader sampling)
     * @param handle Tile handle
     * @return UV offset or (0,0) if invalid
     */
    [[nodiscard]] glm::vec2 getTileUVOffset(AtlasTileHandle handle) const;

    /**
     * @brief Get viewport for rendering to a tile
     * @param handle Tile handle
     * @return Viewport configured for the tile
     */
    [[nodiscard]] VkViewport getTileViewport(AtlasTileHandle handle) const;

    /**
     * @brief Get scissor rect for rendering to a tile
     * @param handle Tile handle
     * @return Scissor rect for the tile
     */
    [[nodiscard]] VkRect2D getTileScissor(AtlasTileHandle handle) const;

    /**
     * @brief Get atlas tile info for shader (UV offset, scale, light-space matrix)
     * @param handle Tile handle
     * @param lightSpaceMatrix Light-space matrix for this light
     * @return AtlasTileInfo for shader uniform
     */
    [[nodiscard]] AtlasTileInfo getTileInfo(AtlasTileHandle handle, const glm::mat4& lightSpaceMatrix) const;

    // =========================================================================
    // Rendering
    // =========================================================================

    /**
     * @brief Begin shadow atlas render pass
     *
     * Clears the entire atlas. Call this once before rendering all tiles.
     *
     * @param cmd Command buffer
     */
    void beginRenderPass(VkCommandBuffer cmd);

    /**
     * @brief End shadow atlas render pass
     * @param cmd Command buffer
     */
    void endRenderPass(VkCommandBuffer cmd);

    /**
     * @brief Set viewport and scissor for a specific tile
     *
     * Call this before rendering geometry for a specific light.
     *
     * @param cmd Command buffer
     * @param handle Tile handle
     */
    void setTileViewportScissor(VkCommandBuffer cmd, AtlasTileHandle handle);

    // =========================================================================
    // Getters
    // =========================================================================

    /**
     * @brief Get atlas depth image view
     */
    [[nodiscard]] VkImageView getImageView() const;

    /**
     * @brief Get shadow sampler
     */
    [[nodiscard]] VkSampler getSampler() const { return shadowSampler; }

    /**
     * @brief Get render pass
     */
    [[nodiscard]] VkRenderPass getRenderPass() const { return renderPass; }

    /**
     * @brief Check if atlas is initialized
     */
    [[nodiscard]] bool isInitialized() const { return initialized; }

private:
    VulkanContext* context{nullptr};
    bool initialized{false};

    // Atlas resources
    std::unique_ptr<OhaoVkImage> atlasImage;
    VkRenderPass renderPass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    VkSampler shadowSampler{VK_NULL_HANDLE};

    // Tile allocation tracking (bitset for fast lookup)
    std::bitset<kTotalTiles> allocatedTiles;

    // =========================================================================
    // Internal methods
    // =========================================================================

    bool createAtlasImage();
    bool createRenderPass();
    bool createFramebuffer();
    bool createShadowSampler();

    /**
     * @brief Convert tile index to row/column
     */
    [[nodiscard]] static constexpr std::pair<uint32_t, uint32_t> tileIndexToRowCol(uint32_t index) {
        return {index / kTilesPerRow, index % kTilesPerRow};
    }

    /**
     * @brief Convert row/column to pixel offset
     */
    [[nodiscard]] static constexpr std::pair<uint32_t, uint32_t> rowColToPixelOffset(uint32_t row, uint32_t col) {
        return {col * kTileSize, row * kTileSize};
    }
};

// =============================================================================
// COMPILE-TIME VALIDATION
// =============================================================================

static_assert(ShadowAtlas::kAtlasSize == 4096, "Atlas size must be 4096");
static_assert(ShadowAtlas::kTileSize == 1024, "Tile size must be 1024");
static_assert(ShadowAtlas::kTilesPerRow == 4, "Must have 4 tiles per row");
static_assert(ShadowAtlas::kTotalTiles == 16, "Must have 16 total tiles");
static_assert(ShadowAtlas::kAtlasSize % ShadowAtlas::kTileSize == 0,
              "Atlas size must be divisible by tile size");

} // namespace ohao
