// math.glsl - Math utilities for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/common/math.glsl

#ifndef OHAO_COMMON_MATH_GLSL
#define OHAO_COMMON_MATH_GLSL

// Include constants for EPSILON and other defines
#include "includes/common/constants.glsl"

// =============================================================================
// Mathematical Constants
// =============================================================================

#define PI          3.14159265359
#define TWO_PI      6.28318530718
#define HALF_PI     1.57079632679
#define INV_PI      0.31830988618
#define INV_TWO_PI  0.15915494309

// Euler's number (named EULER to avoid conflicts with local variable names)
#define EULER       2.71828182846

// Golden ratio
#define PHI         1.61803398875

// =============================================================================
// Clamping Functions
// =============================================================================

// Saturate: clamp to [0, 1] range (HLSL-style)
float saturate(float x) {
    return clamp(x, 0.0, 1.0);
}

vec2 saturate(vec2 x) {
    return clamp(x, vec2(0.0), vec2(1.0));
}

vec3 saturate(vec3 x) {
    return clamp(x, vec3(0.0), vec3(1.0));
}

vec4 saturate(vec4 x) {
    return clamp(x, vec4(0.0), vec4(1.0));
}

// =============================================================================
// Remapping Functions
// =============================================================================

// Remap value from one range to another
float remap(float value, float fromLow, float fromHigh, float toLow, float toHigh) {
    return toLow + (value - fromLow) * (toHigh - toLow) / (fromHigh - fromLow);
}

// Remap and clamp to output range
float remapClamped(float value, float fromLow, float fromHigh, float toLow, float toHigh) {
    float t = saturate((value - fromLow) / (fromHigh - fromLow));
    return mix(toLow, toHigh, t);
}

// Linear step (like smoothstep but linear)
float linearstep(float edge0, float edge1, float x) {
    return saturate((x - edge0) / (edge1 - edge0));
}

// =============================================================================
// Trigonometric Utilities
// =============================================================================

// Fast approximation of atan2
float fastAtan2(float y, float x) {
    float ax = abs(x);
    float ay = abs(y);
    float a = min(ax, ay) / max(ax, ay);
    float s = a * a;
    float r = ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * a + a;
    if (ay > ax) r = HALF_PI - r;
    if (x < 0.0) r = PI - r;
    if (y < 0.0) r = -r;
    return r;
}

// =============================================================================
// Vector Utilities
// =============================================================================

// Square length (avoids sqrt)
float lengthSquared(vec2 v) {
    return dot(v, v);
}

float lengthSquared(vec3 v) {
    return dot(v, v);
}

// Safe normalize (handles zero-length vectors)
vec3 safeNormalize(vec3 v) {
    float len = length(v);
    return len > EPSILON ? v / len : vec3(0.0);
}

// Project vector a onto vector b
vec3 project(vec3 a, vec3 b) {
    return b * (dot(a, b) / dot(b, b));
}

// Reject vector a from vector b (orthogonal component)
vec3 reject(vec3 a, vec3 b) {
    return a - project(a, b);
}

// Reflect vector across normal (built-in exists but included for completeness)
// vec3 reflectVector(vec3 v, vec3 n) {
//     return v - 2.0 * dot(v, n) * n;
// }

// =============================================================================
// Matrix Utilities
// =============================================================================

// Extract rotation matrix from a 4x4 transform (assuming uniform scale)
mat3 getRotationMatrix(mat4 m) {
    return mat3(
        normalize(m[0].xyz),
        normalize(m[1].xyz),
        normalize(m[2].xyz)
    );
}

// =============================================================================
// Interpolation Functions
// =============================================================================

// Smooth Hermite interpolation (same as smoothstep but explicit)
float smoothHermite(float t) {
    return t * t * (3.0 - 2.0 * t);
}

// Smoother Hermite interpolation (Ken Perlin's improved version)
float smootherHermite(float t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// Bias function (attempt to match gamma curve)
float bias(float x, float b) {
    return pow(x, log(b) / log(0.5));
}

// Gain function (attempt to match s-curve)
float gain(float x, float g) {
    if (x < 0.5) {
        return bias(2.0 * x, 1.0 - g) / 2.0;
    } else {
        return 1.0 - bias(2.0 - 2.0 * x, 1.0 - g) / 2.0;
    }
}

// =============================================================================
// Packing/Unpacking
// =============================================================================

// Pack normal from [-1,1] to [0,1]
vec3 packNormal(vec3 n) {
    return n * 0.5 + 0.5;
}

// Unpack normal from [0,1] to [-1,1]
vec3 unpackNormal(vec3 n) {
    return n * 2.0 - 1.0;
}

// Encode octahedral normal (2 components for normal storage)
vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return n.xy * 0.5 + 0.5;
}

// Decode octahedral normal
vec3 octDecode(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.xy, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

#endif // OHAO_COMMON_MATH_GLSL
