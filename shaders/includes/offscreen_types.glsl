// offscreen_types.glsl - Common type definitions for offscreen renderer
// GLSL include file - use with glslangValidator -I flag

#ifndef OFFSCREEN_TYPES_GLSL
#define OFFSCREEN_TYPES_GLSL

// Light types
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2

// Maximum lights (must match C++ MAX_LIGHTS)
#define MAX_LIGHTS 8

// Light data structure (matches C++ LightData - 128 bytes)
struct Light {
    vec4 position;          // xyz = position, w = type
    vec4 direction;         // xyz = direction, w = range
    vec4 color;             // xyz = color, w = intensity
    vec4 params;            // x = innerCone, y = outerCone, z = shadowMapIndex (-1=none), w = unused
    mat4 lightSpaceMatrix;  // Transform to light space for shadow mapping (64 bytes)
};

#endif // OFFSCREEN_TYPES_GLSL
