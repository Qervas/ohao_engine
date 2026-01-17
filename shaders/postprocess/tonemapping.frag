#version 450

// Tonemapping Fragment Shader
// Converts HDR to LDR with various tonemapping operators

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrInput;
layout(set = 0, binding = 1) uniform sampler2D bloomInput;

layout(push_constant) uniform TonemapParams {
    float exposure;
    float gamma;
    float bloomStrength;
    uint tonemapOperator;  // 0=ACES, 1=Reinhard, 2=Uncharted2, 3=Neutral
} params;

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
        default:
            mapped = ACESFilm(hdrColor);
            break;
    }

    // Apply gamma correction
    mapped = pow(mapped, vec3(1.0 / params.gamma));

    outColor = vec4(mapped, 1.0);
}
