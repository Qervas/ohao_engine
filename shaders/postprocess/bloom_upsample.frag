#version 450

// Bloom Upsample Pass
// 9-tap tent filter for smooth upsampling
// Blending with current mip is done via hardware additive blend (ONE, ONE)

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D inputTexture;   // Lower mip (being upsampled)

layout(push_constant) uniform UpsampleParams {
    vec2 texelSize;  // 1.0 / inputResolution (lower mip)
    float filterRadius;
    float blendFactor;  // Used to scale contribution (hardware blend adds to existing)
} params;

void main() {
    vec2 uv = inTexCoord;
    vec2 offset = params.texelSize * params.filterRadius;

    // 9-tap tent filter
    // 1 2 1
    // 2 4 2
    // 1 2 1
    vec3 a = texture(inputTexture, uv + vec2(-offset.x, -offset.y)).rgb;
    vec3 b = texture(inputTexture, uv + vec2(       0.0, -offset.y)).rgb;
    vec3 c = texture(inputTexture, uv + vec2( offset.x, -offset.y)).rgb;

    vec3 d = texture(inputTexture, uv + vec2(-offset.x,        0.0)).rgb;
    vec3 e = texture(inputTexture, uv).rgb;
    vec3 f = texture(inputTexture, uv + vec2( offset.x,        0.0)).rgb;

    vec3 g = texture(inputTexture, uv + vec2(-offset.x,  offset.y)).rgb;
    vec3 h = texture(inputTexture, uv + vec2(       0.0,  offset.y)).rgb;
    vec3 i = texture(inputTexture, uv + vec2( offset.x,  offset.y)).rgb;

    // Weighted sum (tent filter)
    vec3 upsampled = e * 4.0;
    upsampled += (b + d + f + h) * 2.0;
    upsampled += (a + c + g + i);
    upsampled *= (1.0 / 16.0);

    // Scale by blend factor - hardware additive blend adds this to existing framebuffer content
    outColor = vec4(upsampled * params.blendFactor, 1.0);
}
