#version 450

// Particle Render Fragment Shader
// Soft circular particles with alpha blending

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    // Circular particle shape: fade from center
    vec2 center = fragUV - 0.5;
    float dist = length(center) * 2.0; // 0 at center, 1 at edge

    // Smooth circle with soft edge
    float alpha = 1.0 - smoothstep(0.6, 1.0, dist);

    // Apply particle color and alpha
    outColor = vec4(fragColor.rgb, fragColor.a * alpha);

    // Discard fully transparent pixels
    if (outColor.a < 0.01) discard;
}
