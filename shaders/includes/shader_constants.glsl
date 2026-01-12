// shader_constants.glsl - Auto-generated shader constants
// THIS FILE SHOULD BE REGENERATED WHEN shader_bindings.hpp CHANGES
//
// Source of truth: src/renderer/shader/shader_bindings.hpp
// Generator: cmake/GenerateShaderConstants.cmake
//
// DO NOT EDIT MANUALLY - changes will be overwritten

#ifndef SHADER_CONSTANTS_GLSL
#define SHADER_CONSTANTS_GLSL

// =============================================================================
// DESCRIPTOR SET 0: Main Rendering Bindings
// =============================================================================

#define BINDING_GLOBAL_UBO        0
#define BINDING_SHADOW_MAP_ARRAY  1
#define BINDING_SHADOW_ATLAS      2
#define BINDING_CSM_CASCADES      3
#define SET0_BINDING_COUNT        4

// =============================================================================
// DESCRIPTOR SET 1: Per-Material Textures (Future)
// =============================================================================

#define BINDING_ALBEDO_MAP         0
#define BINDING_NORMAL_MAP         1
#define BINDING_METALLIC_ROUGHNESS 2
#define BINDING_AO_MAP             3
#define BINDING_EMISSIVE_MAP       4
#define SET1_BINDING_COUNT         5

// =============================================================================
// ARRAY SIZES - Must match C++ exactly
// =============================================================================

// Maximum number of lights in the scene
#define MAX_LIGHTS        8

// Maximum number of individual shadow maps (legacy system)
#define MAX_SHADOW_MAPS   4

// Number of cascades for CSM directional light shadows
#define MAX_CSM_CASCADES  4

// Maximum number of point/spot lights with shadows in atlas
#define MAX_ATLAS_TILES   16

// =============================================================================
// SHADOW ATLAS CONFIGURATION
// =============================================================================

// Total atlas texture size
#define SHADOW_ATLAS_SIZE      4096

// Size of each shadow tile
#define SHADOW_TILE_SIZE       1024

// Number of tiles per row
#define SHADOW_TILES_PER_ROW   4

// Total number of tiles
#define SHADOW_TOTAL_TILES     16

// UV scale for each tile (1.0 / SHADOW_TILES_PER_ROW)
#define SHADOW_TILE_UV_SCALE   0.25

// =============================================================================
// CASCADED SHADOW MAP CONFIGURATION
// =============================================================================

// Resolution of each cascade shadow map
#define CSM_CASCADE_RESOLUTION  2048

// Default lambda for cascade split (0 = uniform, 1 = logarithmic)
#define CSM_DEFAULT_SPLIT_LAMBDA  0.95

// Maximum shadow distance from camera
#define CSM_DEFAULT_SHADOW_DISTANCE  100.0

// =============================================================================
// PCSS SOFT SHADOW CONFIGURATION
// =============================================================================

// Number of samples for blocker search
#define PCSS_BLOCKER_SEARCH_SAMPLES  16

// Number of samples for PCF filtering
#define PCSS_PCF_SAMPLES  25

// Default light size for penumbra calculation
#define PCSS_DEFAULT_LIGHT_SIZE  0.04

// Maximum penumbra size (in texels)
#define PCSS_MAX_PENUMBRA_SIZE  15.0

// Minimum penumbra size (in texels)
#define PCSS_MIN_PENUMBRA_SIZE  1.0

// =============================================================================
// PUSH CONSTANT CONFIGURATION
// =============================================================================

// Maximum push constant size (Vulkan minimum guarantee)
#define PUSH_CONSTANT_MAX_SIZE     128

// Model matrix offset
#define PUSH_CONSTANT_MODEL_OFFSET 0

// Model matrix size (mat4 = 64 bytes)
#define PUSH_CONSTANT_MODEL_SIZE   64

// Material properties offset
#define PUSH_CONSTANT_MATERIAL_OFFSET  64

// Material properties size
#define PUSH_CONSTANT_MATERIAL_SIZE    32

// =============================================================================
// LIGHT TYPE CONSTANTS
// =============================================================================

#define LIGHT_TYPE_DIRECTIONAL  0.0
#define LIGHT_TYPE_POINT        1.0
#define LIGHT_TYPE_SPOT         2.0

// =============================================================================
// SHADOW TYPE CONSTANTS
// =============================================================================

#define SHADOW_TYPE_NONE      0
#define SHADOW_TYPE_SIMPLE    1
#define SHADOW_TYPE_CASCADED  2
#define SHADOW_TYPE_ATLAS     3
#define SHADOW_TYPE_CUBEMAP   4

#endif // SHADER_CONSTANTS_GLSL
