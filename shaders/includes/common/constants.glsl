// constants.glsl - Engine constants for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/common/constants.glsl
//
// All constants here MUST be kept in sync with the C++ engine code.
// See: src/renderer/offscreen/offscreen_types.hpp

#ifndef OHAO_COMMON_CONSTANTS_GLSL
#define OHAO_COMMON_CONSTANTS_GLSL

// =============================================================================
// Lighting Constants
// =============================================================================

// Maximum number of lights (must match C++ MAX_LIGHTS in offscreen_types.hpp)
#ifndef MAX_LIGHTS
#define MAX_LIGHTS 8
#endif

// Light type identifiers
#define LIGHT_TYPE_DIRECTIONAL 0
#define LIGHT_TYPE_POINT       1
#define LIGHT_TYPE_SPOT        2

// Shadow constants
#define SHADOW_MAP_NONE -1.0
#define MAX_SHADOW_CASCADES 4

// =============================================================================
// Material Constants
// =============================================================================

// Material flags (bitmask)
#define MATERIAL_FLAG_NONE           0x0000
#define MATERIAL_FLAG_HAS_ALBEDO_MAP 0x0001
#define MATERIAL_FLAG_HAS_NORMAL_MAP 0x0002
#define MATERIAL_FLAG_HAS_METALLIC_ROUGHNESS_MAP 0x0004
#define MATERIAL_FLAG_HAS_AO_MAP     0x0008
#define MATERIAL_FLAG_HAS_EMISSIVE_MAP 0x0010
#define MATERIAL_FLAG_ALPHA_BLEND    0x0020
#define MATERIAL_FLAG_DOUBLE_SIDED   0x0040

// Default material values
#define DEFAULT_METALLIC  0.0
#define DEFAULT_ROUGHNESS 0.5
#define DEFAULT_AO        1.0

// =============================================================================
// Rendering Constants
// =============================================================================

// Depth range for Vulkan (0 to 1, unlike OpenGL's -1 to 1)
#define DEPTH_NEAR 0.0
#define DEPTH_FAR  1.0

// Epsilon values for numerical stability
#define EPSILON       1e-6
#define EPSILON_SMALL 1e-10

// Maximum values
#define MAX_REFLECTION_LOD 4.0

#endif // OHAO_COMMON_CONSTANTS_GLSL
