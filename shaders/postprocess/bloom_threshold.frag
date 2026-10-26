#version 450

// Bloom Threshold Pass
// Extracts bright pixels from HDR image

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrInput;

layout(push_constant) uniform BloomParams {
    float threshold;
    float softThreshold;
    float intensity;
    float maxLuminance;   // the input's luminance at most: a brighter source blooms as this bright
} params;

void main() {
    vec3 color = texture(hdrInput, inTexCoord).rgb;

    // Calculate luminance
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // Soft knee threshold
    float soft = luminance - params.threshold + params.softThreshold;
    soft = clamp(soft / (2.0 * params.softThreshold + 0.0001), 0.0, 1.0);
    soft = soft * soft;

    // Contribution based on how much pixel exceeds threshold
    float contribution = max(soft, luminance - params.threshold);
    contribution /= max(luminance, 0.0001);

    // Clamp the extracted luminance: a light's emitter mesh is as bright as the light (tens of
    // thousands), and the mip chain would spread all of it over the frame. Lighting is untouched;
    // only how far its glare reaches.
    vec3 bright = color * contribution;
    const float brightLuminance = luminance * contribution;
    if (brightLuminance > params.maxLuminance) bright *= params.maxLuminance / brightLuminance;
    outColor = vec4(bright, 1.0);
}
