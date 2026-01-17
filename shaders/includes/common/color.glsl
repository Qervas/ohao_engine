// color.glsl - Color space conversion utilities for OHAO Engine shaders
// GLSL include file - use with glslangValidator -I flag
//
// Part of OHAO Engine shader system
// Location: includes/common/color.glsl

#ifndef OHAO_COMMON_COLOR_GLSL
#define OHAO_COMMON_COLOR_GLSL

// =============================================================================
// sRGB <-> Linear Conversion
// =============================================================================

// Convert a single sRGB channel to linear
float sRGBToLinear(float srgb) {
    if (srgb <= 0.04045) {
        return srgb / 12.92;
    } else {
        return pow((srgb + 0.055) / 1.055, 2.4);
    }
}

// Convert sRGB color to linear
vec3 sRGBToLinear(vec3 srgb) {
    return vec3(
        sRGBToLinear(srgb.r),
        sRGBToLinear(srgb.g),
        sRGBToLinear(srgb.b)
    );
}

// Convert sRGB color with alpha to linear (alpha stays linear)
vec4 sRGBToLinear(vec4 srgba) {
    return vec4(sRGBToLinear(srgba.rgb), srgba.a);
}

// Convert a single linear channel to sRGB
float linearToSRGB(float linear) {
    if (linear <= 0.0031308) {
        return linear * 12.92;
    } else {
        return 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
    }
}

// Convert linear color to sRGB
vec3 linearToSRGB(vec3 linear) {
    return vec3(
        linearToSRGB(linear.r),
        linearToSRGB(linear.g),
        linearToSRGB(linear.b)
    );
}

// Convert linear color with alpha to sRGB (alpha stays linear)
vec4 linearToSRGB(vec4 linear) {
    return vec4(linearToSRGB(linear.rgb), linear.a);
}

// =============================================================================
// Fast Approximations (for real-time rendering)
// =============================================================================

// Fast sRGB to linear approximation using gamma 2.2
vec3 sRGBToLinearFast(vec3 srgb) {
    return pow(srgb, vec3(2.2));
}

// Fast linear to sRGB approximation using gamma 2.2
vec3 linearToSRGBFast(vec3 linear) {
    return pow(linear, vec3(1.0 / 2.2));
}

// =============================================================================
// Luminance Calculations
// =============================================================================

// Calculate luminance (perceived brightness) of a linear RGB color
// Using Rec. 709 coefficients
float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// Alternative luminance using Rec. 601 coefficients (more perceptually uniform)
float luminanceRec601(vec3 color) {
    return dot(color, vec3(0.299, 0.587, 0.114));
}

// =============================================================================
// HDR / Tonemapping Utilities
// =============================================================================

// Reinhard tonemapping (simple)
vec3 tonemapReinhard(vec3 hdr) {
    return hdr / (hdr + vec3(1.0));
}

// Extended Reinhard tonemapping with white point
vec3 tonemapReinhardExtended(vec3 hdr, float whitePoint) {
    vec3 numerator = hdr * (1.0 + hdr / (whitePoint * whitePoint));
    return numerator / (1.0 + hdr);
}

// ACES Filmic Tone Mapping (approximation by Krzysztof Narkowicz)
vec3 tonemapACES(vec3 hdr) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((hdr * (a * hdr + b)) / (hdr * (c * hdr + d) + e), 0.0, 1.0);
}

// Uncharted 2 tonemapping (John Hable's filmic curve)
vec3 uncharted2Tonemap(vec3 x) {
    const float A = 0.15; // Shoulder Strength
    const float B = 0.50; // Linear Strength
    const float C = 0.10; // Linear Angle
    const float D = 0.20; // Toe Strength
    const float E = 0.02; // Toe Numerator
    const float F = 0.30; // Toe Denominator
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 tonemapUncharted2(vec3 hdr, float exposureBias) {
    const float W = 11.2; // Linear White Point
    vec3 curr = uncharted2Tonemap(exposureBias * hdr);
    vec3 whiteScale = 1.0 / uncharted2Tonemap(vec3(W));
    return curr * whiteScale;
}

// =============================================================================
// Color Grading Utilities
// =============================================================================

// Adjust saturation (0 = grayscale, 1 = original, >1 = oversaturated)
vec3 adjustSaturation(vec3 color, float saturation) {
    float luma = luminance(color);
    return mix(vec3(luma), color, saturation);
}

// Adjust contrast (0 = gray, 1 = original, >1 = high contrast)
vec3 adjustContrast(vec3 color, float contrast) {
    return (color - 0.5) * contrast + 0.5;
}

// Adjust brightness (additive)
vec3 adjustBrightness(vec3 color, float brightness) {
    return color + brightness;
}

// Adjust exposure (multiplicative, in stops)
vec3 adjustExposure(vec3 color, float exposureStops) {
    return color * pow(2.0, exposureStops);
}

// =============================================================================
// Color Space Conversion
// =============================================================================

// RGB to HSV
vec3 rgbToHsv(vec3 rgb) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(rgb.bg, K.wz), vec4(rgb.gb, K.xy), step(rgb.b, rgb.g));
    vec4 q = mix(vec4(p.xyw, rgb.r), vec4(rgb.r, p.yzx), step(p.x, rgb.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// HSV to RGB
vec3 hsvToRgb(vec3 hsv) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(hsv.xxx + K.xyz) * 6.0 - K.www);
    return hsv.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), hsv.y);
}

#endif // OHAO_COMMON_COLOR_GLSL
