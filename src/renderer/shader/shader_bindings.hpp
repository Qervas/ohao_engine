#pragma once
/**
 * shader_bindings.hpp - Single Source of Truth for Shader Constants
 *
 * CRITICAL: This file defines ALL binding indices, array sizes, and configuration
 * constants shared between C++ and GLSL. Changes here MUST be synchronized with
 * shaders/includes/shader_constants.glsl (auto-generated via CMake).
 *
 * COMPILE-TIME SAFETY:
 * - All values are constexpr for compile-time validation
 * - static_assert guards prevent silent mismatches
 * - Namespace organization prevents naming conflicts
 */

#include <cstdint>

namespace ohao {
namespace ShaderBindings {

// =============================================================================
// DESCRIPTOR SET 0: Main Rendering Bindings
// =============================================================================
namespace Set0 {
    /// @brief Global uniform buffer (camera, lights, materials)
    constexpr uint32_t kGlobalUBO         = 0;

    /// @brief Legacy shadow map array (for backward compatibility during migration)
    constexpr uint32_t kShadowMapArray    = 1;

    /// @brief Shadow atlas for local lights (4096x4096, 16 tiles)
    constexpr uint32_t kShadowAtlas       = 2;

    /// @brief Cascaded shadow map array for directional light (4 cascades)
    constexpr uint32_t kCSMCascades       = 3;

    /// @brief Total number of bindings in set 0
    constexpr uint32_t kBindingCount      = 4;

    // Compile-time validation: ensure bindings are unique and sequential
    static_assert(kGlobalUBO == 0, "GlobalUBO must be binding 0");
    static_assert(kShadowMapArray == 1, "ShadowMapArray must be binding 1");
    static_assert(kShadowAtlas == 2, "ShadowAtlas must be binding 2");
    static_assert(kCSMCascades == 3, "CSMCascades must be binding 3");
}

// =============================================================================
// DESCRIPTOR SET 1: Per-Material Textures (Future)
// =============================================================================
namespace Set1 {
    constexpr uint32_t kAlbedoMap         = 0;
    constexpr uint32_t kNormalMap         = 1;
    constexpr uint32_t kMetallicRoughness = 2;
    constexpr uint32_t kAOMap             = 3;
    constexpr uint32_t kEmissiveMap       = 4;
    constexpr uint32_t kBindingCount      = 5;
}

// =============================================================================
// ARRAY SIZES - Must match GLSL exactly
// =============================================================================

/// @brief Maximum number of lights in the scene
constexpr int32_t kMaxLights              = 8;

/// @brief Maximum number of individual shadow maps (legacy system)
constexpr int32_t kMaxShadowMaps          = 4;

/// @brief Number of cascades for CSM directional light shadows
constexpr int32_t kMaxCSMCascades         = 4;

/// @brief Maximum number of point/spot lights with shadows in atlas
constexpr int32_t kMaxAtlasTiles          = 16;

// =============================================================================
// SHADOW ATLAS CONFIGURATION
// =============================================================================
namespace ShadowAtlas {
    /// @brief Total atlas texture size (4096x4096)
    constexpr uint32_t kAtlasSize         = 4096;

    /// @brief Size of each shadow tile (1024x1024)
    constexpr uint32_t kTileSize          = 1024;

    /// @brief Number of tiles per row (4)
    constexpr uint32_t kTilesPerRow       = kAtlasSize / kTileSize;

    /// @brief Total number of tiles (16)
    constexpr uint32_t kTotalTiles        = kTilesPerRow * kTilesPerRow;

    /// @brief UV scale for each tile (0.25)
    constexpr float kTileUVScale          = 1.0f / static_cast<float>(kTilesPerRow);

    // Compile-time validation
    static_assert(kAtlasSize % kTileSize == 0, "Atlas size must be divisible by tile size");
    static_assert(kTotalTiles == 16, "Atlas should have exactly 16 tiles");
    static_assert(kTotalTiles >= kMaxAtlasTiles, "Not enough tiles for max atlas lights");
}

// =============================================================================
// CASCADED SHADOW MAP CONFIGURATION
// =============================================================================
namespace CSM {
    /// @brief Resolution of each cascade shadow map
    constexpr uint32_t kCascadeResolution = 2048;

    /// @brief Default lambda for cascade split (0 = uniform, 1 = logarithmic)
    constexpr float kDefaultSplitLambda   = 0.95f;

    /// @brief Maximum shadow distance from camera
    constexpr float kDefaultShadowDistance = 100.0f;

    /// @brief Near clip plane for shadow projection
    constexpr float kDefaultNearClip      = 0.1f;

    /// @brief Far clip plane for shadow projection
    constexpr float kDefaultFarClip       = 200.0f;

    // Compile-time validation
    static_assert(kCascadeResolution >= 1024, "Cascade resolution too low for quality shadows");
    static_assert(kMaxCSMCascades == 4, "CSM system designed for 4 cascades");
}

// =============================================================================
// PCSS SOFT SHADOW CONFIGURATION
// =============================================================================
namespace PCSS {
    /// @brief Number of samples for blocker search
    constexpr int32_t kBlockerSearchSamples = 16;

    /// @brief Number of samples for PCF filtering
    constexpr int32_t kPCFSamples         = 25;

    /// @brief Default light size for penumbra calculation
    constexpr float kDefaultLightSize     = 0.04f;

    /// @brief Maximum penumbra size (in texels)
    constexpr float kMaxPenumbraSize      = 15.0f;

    /// @brief Minimum penumbra size (in texels)
    constexpr float kMinPenumbraSize      = 1.0f;
}

// =============================================================================
// PUSH CONSTANT RANGES
// =============================================================================
namespace PushConstants {
    /// @brief Maximum push constant size (Vulkan minimum guarantee is 128 bytes)
    constexpr uint32_t kMaxSize           = 128;

    /// @brief Model matrix push constant offset
    constexpr uint32_t kModelMatrixOffset = 0;

    /// @brief Model matrix size (64 bytes for mat4)
    constexpr uint32_t kModelMatrixSize   = 64;

    /// @brief Material properties offset
    constexpr uint32_t kMaterialOffset    = 64;

    /// @brief Material properties size
    constexpr uint32_t kMaterialSize      = 32;

    // Validation
    static_assert(kModelMatrixOffset + kModelMatrixSize <= kMaxSize,
                  "Model matrix exceeds push constant size");
    static_assert(kMaterialOffset + kMaterialSize <= kMaxSize,
                  "Material data exceeds push constant size");
}

// =============================================================================
// GLOBAL VALIDATION
// =============================================================================

// Ensure critical constants haven't been changed without updating GLSL
static_assert(kMaxLights == 8,
    "MAX_LIGHTS changed! Update shaders/includes/shader_constants.glsl");
static_assert(kMaxShadowMaps == 4,
    "MAX_SHADOW_MAPS changed! Update shaders/includes/shader_constants.glsl");
static_assert(kMaxCSMCascades == 4,
    "MAX_CSM_CASCADES changed! Update shaders/includes/shader_constants.glsl");
static_assert(ShadowAtlas::kAtlasSize == 4096,
    "SHADOW_ATLAS_SIZE changed! Update shaders/includes/shader_constants.glsl");

} // namespace ShaderBindings
} // namespace ohao
