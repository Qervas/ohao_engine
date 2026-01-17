// types.glsl - Common type definitions for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/common/types.glsl

#ifndef OHAO_COMMON_TYPES_GLSL
#define OHAO_COMMON_TYPES_GLSL

// Light types
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2

// Maximum lights (must match C++ MAX_LIGHTS)
#define MAX_LIGHTS 8

// Light data structure (matches C++ LightData - 128 bytes, std140 layout)
struct Light {
    vec4 position;          // xyz = position, w = type
    vec4 direction;         // xyz = direction, w = range
    vec4 color;             // xyz = color, w = intensity
    vec4 params;            // x = innerCone, y = outerCone, z = shadowMapIndex (-1=none), w = unused
    mat4 lightSpaceMatrix;  // Transform to light space for shadow mapping (64 bytes)
};

#endif // OHAO_COMMON_TYPES_GLSL
