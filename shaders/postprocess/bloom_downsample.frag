#version 450

// Bloom Downsample Pass
// 13-tap filter for high-quality downsampling (prevents aliasing)
// First pass uses Karis average to suppress fireflies from bright point sources
// (CoD: Advanced Warfare, SIGGRAPH 2014 — prevents bloom flickering/dark spots)

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D inputTexture;

layout(push_constant) uniform DownsampleParams {
    vec2 texelSize;     // 1.0 / inputResolution
    float filterRadius; // (unused in downsample, used by upsample)
    float isFirstPass;  // > 0.5 = apply Karis average (anti-firefly)
} params;

// Karis weight: 1/(brightness+1) — bright pixels get lower weight
float karisWeight(vec3 c) {
    return 1.0 / (max(max(c.r, c.g), c.b) + 1.0);
}

void main() {
    vec2 uv = inTexCoord;
    vec2 texel = params.texelSize;

    // 13-tap filter with 4x4 box filter weights
    //
    // Layout:
    // a - b - c
    // - d - e -
    // f - g - h
    // - i - j -
    // k - l - m

    vec3 a = texture(inputTexture, uv + texel * vec2(-2, -2)).rgb;
    vec3 b = texture(inputTexture, uv + texel * vec2( 0, -2)).rgb;
    vec3 c = texture(inputTexture, uv + texel * vec2( 2, -2)).rgb;

    vec3 d = texture(inputTexture, uv + texel * vec2(-1, -1)).rgb;
    vec3 e = texture(inputTexture, uv + texel * vec2( 1, -1)).rgb;

    vec3 f = texture(inputTexture, uv + texel * vec2(-2,  0)).rgb;
    vec3 g = texture(inputTexture, uv).rgb;
    vec3 h = texture(inputTexture, uv + texel * vec2( 2,  0)).rgb;

    vec3 i = texture(inputTexture, uv + texel * vec2(-1,  1)).rgb;
    vec3 j = texture(inputTexture, uv + texel * vec2( 1,  1)).rgb;

    vec3 k = texture(inputTexture, uv + texel * vec2(-2,  2)).rgb;
    vec3 l = texture(inputTexture, uv + texel * vec2( 0,  2)).rgb;
    vec3 m = texture(inputTexture, uv + texel * vec2( 2,  2)).rgb;

    vec3 bloom;

    if (params.isFirstPass > 0.5) {
        // --- Karis-weighted first downsample ---
        // Bright pixels (moon disc, sun, specular highlights) get suppressed
        // proportionally to their brightness, preventing them from overwhelming
        // the downsample and creating uneven bloom halos.

        // Compute group averages (4 samples each)
        vec3 g1 = (a + b + f + g) * 0.25;
        vec3 g2 = (b + c + g + h) * 0.25;
        vec3 g3 = (f + g + k + l) * 0.25;
        vec3 g4 = (g + h + l + m) * 0.25;
        vec3 g5 = (d + e + i + j) * 0.25;

        // Per-group Karis weights
        float w1 = karisWeight(g1);
        float w2 = karisWeight(g2);
        float w3 = karisWeight(g3);
        float w4 = karisWeight(g4);
        float w5 = karisWeight(g5);

        // Apply original relative weights (groups 1-4: 0.125, group 5: 0.5)
        // modulated by Karis, then renormalize to preserve energy
        float kw1 = 0.125 * w1, kw2 = 0.125 * w2;
        float kw3 = 0.125 * w3, kw4 = 0.125 * w4;
        float kw5 = 0.5 * w5;

        bloom = (g1 * kw1 + g2 * kw2 + g3 * kw3 + g4 * kw4 + g5 * kw5) /
                (kw1 + kw2 + kw3 + kw4 + kw5);
    } else {
        // --- Standard 13-tap downsample (subsequent mip levels) ---
        vec3 group1 = (a + b + f + g) * 0.03125; // 1/32
        vec3 group2 = (b + c + g + h) * 0.03125;
        vec3 group3 = (f + g + k + l) * 0.03125;
        vec3 group4 = (g + h + l + m) * 0.03125;
        vec3 group5 = (d + e + i + j) * 0.125;   // 1/8

        bloom = group1 + group2 + group3 + group4 + group5;
    }

    outColor = vec4(bloom, 1.0);
}
