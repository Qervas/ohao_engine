#pragma once
/**
 * compile_time_tests.hpp - Compile-Time Safety Tests for Shadow System
 *
 * This header contains static_assert tests that validate:
 * - Struct layouts match GPU expectations
 * - Handle types cannot be confused
 * - Binding indices are correct
 * - Array sizes are consistent
 *
 * If any test fails, the build will fail with a clear error message.
 * Include this header in at least one translation unit to run all tests.
 */

#include "renderer/shader/shader_bindings.hpp"
#include "renderer/lighting/unified_light.hpp"
#include "renderer/rhi/vk/ohao_vk_descriptor_builder.hpp"

namespace ohao::tests {

// =============================================================================
// HANDLE TYPE SAFETY TESTS
// =============================================================================

// Verify handles cannot be implicitly converted to each other
static_assert(!std::is_convertible_v<LightHandle, ShadowMapHandle>,
              "LightHandle must not convert to ShadowMapHandle");
static_assert(!std::is_convertible_v<ShadowMapHandle, LightHandle>,
              "ShadowMapHandle must not convert to LightHandle");
static_assert(!std::is_convertible_v<CascadeIndex, AtlasTileHandle>,
              "CascadeIndex must not convert to AtlasTileHandle");
static_assert(!std::is_convertible_v<AtlasTileHandle, CascadeIndex>,
              "AtlasTileHandle must not convert to CascadeIndex");

// Verify handles cannot be assigned from each other
static_assert(!std::is_assignable_v<LightHandle, ShadowMapHandle>,
              "LightHandle must not be assignable from ShadowMapHandle");
static_assert(!std::is_assignable_v<ShadowMapHandle, LightHandle>,
              "ShadowMapHandle must not be assignable from LightHandle");

// Verify handles are explicitly constructible from uint32_t
static_assert(std::is_constructible_v<LightHandle, uint32_t>,
              "LightHandle must be constructible from uint32_t");
static_assert(std::is_constructible_v<ShadowMapHandle, uint32_t>,
              "ShadowMapHandle must be constructible from uint32_t");

// Verify handles are NOT implicitly constructible from uint32_t
static_assert(!std::is_convertible_v<uint32_t, LightHandle>,
              "LightHandle must not be implicitly constructible from uint32_t");
static_assert(!std::is_convertible_v<uint32_t, ShadowMapHandle>,
              "ShadowMapHandle must not be implicitly constructible from uint32_t");

// =============================================================================
// STRUCT LAYOUT TESTS
// =============================================================================

// UnifiedLight must be exactly 128 bytes for GPU alignment
static_assert(sizeof(UnifiedLight) == 128,
              "UnifiedLight must be exactly 128 bytes");

// Verify UnifiedLight field offsets match GPU expectations
static_assert(offsetof(UnifiedLight, position) == 0,
              "UnifiedLight::position must be at offset 0");
static_assert(offsetof(UnifiedLight, type) == 12,
              "UnifiedLight::type must be at offset 12");
static_assert(offsetof(UnifiedLight, color) == 16,
              "UnifiedLight::color must be at offset 16");
static_assert(offsetof(UnifiedLight, intensity) == 28,
              "UnifiedLight::intensity must be at offset 28");
static_assert(offsetof(UnifiedLight, direction) == 32,
              "UnifiedLight::direction must be at offset 32");
static_assert(offsetof(UnifiedLight, range) == 44,
              "UnifiedLight::range must be at offset 44");
static_assert(offsetof(UnifiedLight, innerCone) == 48,
              "UnifiedLight::innerCone must be at offset 48");
static_assert(offsetof(UnifiedLight, outerCone) == 52,
              "UnifiedLight::outerCone must be at offset 52");
static_assert(offsetof(UnifiedLight, shadowMapIndex) == 56,
              "UnifiedLight::shadowMapIndex must be at offset 56");
static_assert(offsetof(UnifiedLight, lightSpaceMatrix) == 64,
              "UnifiedLight::lightSpaceMatrix must be at offset 64");

// LightingUBO must match expected size
static_assert(sizeof(LightingUBO) == 1040,
              "LightingUBO size mismatch - check alignment");

// CSMUBO must match expected size
static_assert(sizeof(CSMUBO) == 416,
              "CSMUBO size mismatch - check alignment");

// CSMCascadeInfo must be 80 bytes for std140
static_assert(sizeof(CSMCascadeInfo) == 80,
              "CSMCascadeInfo must be 80 bytes for std140");

// AtlasTileInfo must be 80 bytes for std140
static_assert(sizeof(AtlasTileInfo) == 80,
              "AtlasTileInfo must be 80 bytes for std140");

// =============================================================================
// BINDING INDEX TESTS
// =============================================================================

// Verify descriptor set 0 bindings are sequential
static_assert(ShaderBindings::Set0::kGlobalUBO == 0,
              "GlobalUBO must be binding 0");
static_assert(ShaderBindings::Set0::kShadowMapArray == 1,
              "ShadowMapArray must be binding 1");
static_assert(ShaderBindings::Set0::kShadowAtlas == 2,
              "ShadowAtlas must be binding 2");
static_assert(ShaderBindings::Set0::kCSMCascades == 3,
              "CSMCascades must be binding 3");
static_assert(ShaderBindings::Set0::kBindingCount == 4,
              "Set0 binding count must be 4");

// Verify descriptor builder matches ShaderBindings
static_assert(MainDescriptorSet::GlobalUBO::binding == ShaderBindings::Set0::kGlobalUBO,
              "Descriptor builder GlobalUBO binding mismatch");
static_assert(MainDescriptorSet::ShadowMapArray::binding == ShaderBindings::Set0::kShadowMapArray,
              "Descriptor builder ShadowMapArray binding mismatch");
static_assert(MainDescriptorSet::ShadowAtlas::binding == ShaderBindings::Set0::kShadowAtlas,
              "Descriptor builder ShadowAtlas binding mismatch");
static_assert(MainDescriptorSet::CSMCascades::binding == ShaderBindings::Set0::kCSMCascades,
              "Descriptor builder CSMCascades binding mismatch");

// =============================================================================
// ARRAY SIZE TESTS
// =============================================================================

// Verify array sizes match between descriptor builder and ShaderBindings
static_assert(MainDescriptorSet::ShadowMapArray::descriptorCount == ShaderBindings::kMaxShadowMaps,
              "ShadowMapArray count must match kMaxShadowMaps");
static_assert(MainDescriptorSet::CSMCascades::descriptorCount == ShaderBindings::kMaxCSMCascades,
              "CSMCascades count must match kMaxCSMCascades");

// Verify shadow atlas configuration is consistent
static_assert(ShaderBindings::ShadowAtlas::kAtlasSize == 4096,
              "Shadow atlas size must be 4096");
static_assert(ShaderBindings::ShadowAtlas::kTileSize == 1024,
              "Shadow tile size must be 1024");
static_assert(ShaderBindings::ShadowAtlas::kTilesPerRow == 4,
              "Shadow tiles per row must be 4");
static_assert(ShaderBindings::ShadowAtlas::kTotalTiles == 16,
              "Total shadow tiles must be 16");
static_assert(ShaderBindings::ShadowAtlas::kAtlasSize % ShaderBindings::ShadowAtlas::kTileSize == 0,
              "Atlas size must be divisible by tile size");

// Verify CSM configuration
static_assert(ShaderBindings::kMaxCSMCascades == 4,
              "CSM must have 4 cascades");
static_assert(ShaderBindings::CSM::kCascadeResolution >= 1024,
              "CSM cascade resolution must be at least 1024");

// =============================================================================
// CONSTANTS CONSISTENCY TESTS
// =============================================================================

// Verify unified light constants
static_assert(MAX_UNIFIED_LIGHTS == ShaderBindings::kMaxLights,
              "MAX_UNIFIED_LIGHTS must match kMaxLights");
static_assert(MAX_SHADOW_MAPS == ShaderBindings::kMaxShadowMaps,
              "MAX_SHADOW_MAPS must match kMaxShadowMaps");
static_assert(MAX_CSM_CASCADES == ShaderBindings::kMaxCSMCascades,
              "MAX_CSM_CASCADES must match kMaxCSMCascades");
static_assert(MAX_ATLAS_TILES == ShaderBindings::kMaxAtlasTiles,
              "MAX_ATLAS_TILES must match kMaxAtlasTiles");

// =============================================================================
// PUSH CONSTANT SIZE TESTS
// =============================================================================

static_assert(ShaderBindings::PushConstants::kMaxSize <= 128,
              "Push constant size must not exceed Vulkan minimum guarantee");
static_assert(ShaderBindings::PushConstants::kModelMatrixOffset + ShaderBindings::PushConstants::kModelMatrixSize
              <= ShaderBindings::PushConstants::kMaxSize,
              "Model matrix exceeds push constant size");
static_assert(ShaderBindings::PushConstants::kMaterialOffset + ShaderBindings::PushConstants::kMaterialSize
              <= ShaderBindings::PushConstants::kMaxSize,
              "Material data exceeds push constant size");

} // namespace ohao::tests
