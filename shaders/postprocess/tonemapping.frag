#version 450

// Tonemapping Fragment Shader
// Converts HDR to LDR with various tonemapping operators

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrInput;
layout(set = 0, binding = 1) uniform sampler2D bloomInput;
// Screen-space reflections at half resolution (postprocess/ssr.slang): rgb the reflection,
// a the view depth of the pixel it was traced for (0: sky). Sample q sits on pixel 2q.
layout(set = 0, binding = 2) uniform sampler2D ssrInput;
layout(set = 0, binding = 3) uniform sampler2D depthInput;   // the G-buffer depth: the upsample's guide

layout(push_constant) uniform TonemapParams {
    float exposure;
    float gamma;
    float bloomStrength;
    uint tonemapOperator;  // 0=ACES, 1=Reinhard, 2=Uncharted2, 3=Neutral
    float flashIntensity;  // lightning flash — additive pre-tonemap HDR brightening
    uint ssrOn;            // 1: add the SSR
    float depthA;          // projection [2][2], [3][2]: view depth = depthB / (d + depthA)
    float depthB;
} params;

// The SSR at this pixel: the 4 nearest half-resolution samples, weighted bilinearly and by how
// close their view depth is to this pixel's (within 5%); where none is the same surface, the
// closest. The depth is read only where a neighbour reflects anything.
vec3 ssrAt(ivec2 pixel) {
    const ivec2 size = textureSize(ssrInput, 0);
    const ivec2 base = pixel >> 1;
    vec4 t[4];
    bool any = false;
    for (int i = 0; i < 4; ++i) {
        t[i] = texelFetch(ssrInput, min(base + ivec2(i & 1, i >> 1), size - 1), 0);
        any = any || (t[i].r + t[i].g + t[i].b) > 0.0;
    }
    if (!any) return vec3(0.0);
    const float d = texelFetch(depthInput, pixel, 0).r;
    if (d >= 1.0) return vec3(0.0);
    const float z = params.depthB / (d + params.depthA);
    const vec2 f = vec2(pixel & 1) * 0.5;
    vec3 sum = vec3(0.0), best = vec3(0.0);
    float wsum = 0.0, bestErr = 1e30;
    for (int i = 0; i < 4; ++i) {
        if (t[i].a <= 0.0) continue;
        const ivec2 o = ivec2(i & 1, i >> 1);
        const float err = abs(t[i].a - z);
        if (err < bestErr) { bestErr = err; best = t[i].rgb; }
        const float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) * max(0.0, 1.0 - err / (0.05 * z));
        sum += w * t[i].rgb;
        wsum += w;
    }
    return wsum > 1e-3 ? sum / wsum : best;
}

// ACES filmic tonemapping
vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Reinhard extended tonemapping
vec3 ReinhardExtended(vec3 x, float whitePoint) {
    vec3 numerator = x * (1.0 + x / (whitePoint * whitePoint));
    return numerator / (1.0 + x);
}

// Uncharted 2 tonemapping
vec3 Uncharted2Tonemap(vec3 x) {
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 Uncharted2(vec3 x) {
    float exposureBias = 2.0;
    vec3 curr = Uncharted2Tonemap(exposureBias * x);
    vec3 whiteScale = 1.0 / Uncharted2Tonemap(vec3(11.2));
    return curr * whiteScale;
}

// Filmic tonemapping — softer highlight rolloff than ACES, better mid-tone contrast
// Based on Jim Hejl's filmic curve (used in many AAA games)
vec3 FilmicTonemap(vec3 x) {
    // Pre-curve lift to preserve shadow detail
    x = max(vec3(0.0), x - 0.004);
    // Filmic S-curve with gentle highlight compression
    x = (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
    // Result is already in gamma space — no need for gamma correction after this
    return x;
}

// Neutral tonemapping (logarithmic)
vec3 NeutralTonemap(vec3 x) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float peak = max(x.r, max(x.g, x.b));
    if (peak < startCompression) {
        return x;
    }

    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    x *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(x, newPeak * vec3(1.0), g);
}

void main() {
    // Sample HDR color
    vec3 hdrColor = texture(hdrInput, inTexCoord).rgb;

    // Add bloom
    vec3 bloom = texture(bloomInput, inTexCoord).rgb;
    hdrColor += bloom * params.bloomStrength;

    // Add screen-space reflections
    if (params.ssrOn != 0u) hdrColor += ssrAt(ivec2(gl_FragCoord.xy));

    // Lightning flash — warm-white additive HDR brightening (tonemapper compresses it naturally)
    if (params.flashIntensity > 0.001) {
        hdrColor += params.flashIntensity * vec3(1.0, 0.97, 0.88);
    }

    // Apply exposure
    hdrColor *= params.exposure;

    // Apply tonemapping
    vec3 mapped;
    switch (params.tonemapOperator) {
        case 0:
            mapped = ACESFilm(hdrColor);
            break;
        case 1:
            mapped = ReinhardExtended(hdrColor, 4.0);
            break;
        case 2:
            mapped = Uncharted2(hdrColor);
            break;
        case 3:
            mapped = NeutralTonemap(hdrColor);
            break;
        case 4:
            mapped = FilmicTonemap(hdrColor);
            break;
        default:
            mapped = ACESFilm(hdrColor);
            break;
    }

    // Apply gamma correction (Filmic curve already outputs gamma-space)
    if (params.tonemapOperator != 4u) {
        mapped = pow(mapped, vec3(1.0 / params.gamma));
    }

    outColor = vec4(mapped, 1.0);
}
