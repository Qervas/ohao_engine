#version 450

// Bloom Downsample Pass
// 13-tap filter for high-quality downsampling (prevents aliasing)

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D inputTexture;

layout(push_constant) uniform DownsampleParams {
    vec2 texelSize;  // 1.0 / inputResolution
} params;

void main() {
    vec2 uv = inTexCoord;
    vec2 texel = params.texelSize;

    // 13-tap filter with 4x4 box filter weights
    // Reduces high frequency artifacts during downsampling
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

    // Weight groups
    vec3 group1 = (a + b + f + g) * 0.03125; // 1/32
    vec3 group2 = (b + c + g + h) * 0.03125;
    vec3 group3 = (f + g + k + l) * 0.03125;
    vec3 group4 = (g + h + l + m) * 0.03125;
    vec3 group5 = (d + e + i + j) * 0.125;   // 1/8

    vec3 bloom = group1 + group2 + group3 + group4 + group5;

    outColor = vec4(bloom, 1.0);
}
