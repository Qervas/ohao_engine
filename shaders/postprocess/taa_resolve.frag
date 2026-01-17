#version 450

// Temporal Anti-Aliasing Resolve Shader
// Blends current frame with history buffer using motion vectors

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D currentFrame;
layout(set = 0, binding = 1) uniform sampler2D historyFrame;
layout(set = 0, binding = 2) uniform sampler2D velocityBuffer;
layout(set = 0, binding = 3) uniform sampler2D depthBuffer;

layout(push_constant) uniform TAAParams {
    vec2 texelSize;
    float blendFactor;     // Base blend factor (higher = more history)
    float motionScale;     // Velocity influence on blend
    uint flags;            // Bit 0: useMotionVectors, Bit 1: useVarianceClipping
} params;

// Convert RGB to YCoCg color space for neighborhood clamping
vec3 RGBToYCoCg(vec3 rgb) {
    return vec3(
         0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b,
         0.5 * rgb.r                 - 0.5 * rgb.b,
        -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b
    );
}

vec3 YCoCgToRGB(vec3 YCoCg) {
    return vec3(
        YCoCg.x + YCoCg.y - YCoCg.z,
        YCoCg.x           + YCoCg.z,
        YCoCg.x - YCoCg.y - YCoCg.z
    );
}

// Neighborhood clamping in YCoCg space
vec3 clipToAABB(vec3 aabbMin, vec3 aabbMax, vec3 prevSample) {
    vec3 pClip = 0.5 * (aabbMax + aabbMin);
    vec3 eClip = 0.5 * (aabbMax - aabbMin) + 0.001;

    vec3 vClip = prevSample - pClip;
    vec3 vUnit = vClip / eClip;
    vec3 aUnit = abs(vUnit);
    float maUnit = max(aUnit.x, max(aUnit.y, aUnit.z));

    if (maUnit > 1.0) {
        return pClip + vClip / maUnit;
    } else {
        return prevSample;
    }
}

// Sample with catmull-rom filtering for sharper history
vec3 sampleHistory(vec2 uv) {
    vec2 samplePos = uv / params.texelSize - 0.5;
    vec2 texPos = floor(samplePos);
    vec2 f = samplePos - texPos;
    texPos = (texPos + 0.5) * params.texelSize;

    // Catmull-Rom weights
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    // Optimized 4-tap bilinear fetches
    vec2 s0 = w0 + w1;
    vec2 s1 = w2 + w3;

    vec2 f0 = w1 / s0;
    vec2 f1 = w3 / s1;

    vec2 t0 = texPos - params.texelSize * (1.0 - f0);
    vec2 t1 = texPos + params.texelSize * (1.0 + f1);

    vec3 result =
        (texture(historyFrame, vec2(t0.x, t0.y)).rgb * s0.x +
         texture(historyFrame, vec2(t1.x, t0.y)).rgb * s1.x) * s0.y +
        (texture(historyFrame, vec2(t0.x, t1.y)).rgb * s0.x +
         texture(historyFrame, vec2(t1.x, t1.y)).rgb * s1.x) * s1.y;

    return max(result, vec3(0.0));
}

void main() {
    vec2 uv = inTexCoord;

    // Sample current frame
    vec3 currentColor = texture(currentFrame, uv).rgb;

    // Get velocity from motion vectors
    vec2 velocity = vec2(0.0);
    if ((params.flags & 1u) != 0u) {
        velocity = texture(velocityBuffer, uv).rg;
    }

    // Reproject to previous frame position
    vec2 historyUV = uv - velocity;

    // Check if reprojected position is valid
    bool validHistory = historyUV.x >= 0.0 && historyUV.x <= 1.0 &&
                        historyUV.y >= 0.0 && historyUV.y <= 1.0;

    if (!validHistory) {
        outColor = vec4(currentColor, 1.0);
        return;
    }

    // Sample history with filtering
    vec3 historyColor = sampleHistory(historyUV);

    // Neighborhood sampling for color clamping
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);

    // 3x3 neighborhood
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 c = texture(currentFrame, uv + vec2(x, y) * params.texelSize).rgb;
            c = RGBToYCoCg(c);
            m1 += c;
            m2 += c * c;
        }
    }

    m1 /= 9.0;
    m2 /= 9.0;

    // Variance-based clipping
    vec3 sigma = sqrt(max(m2 - m1 * m1, vec3(0.0)));
    float varClipGamma = 1.0;

    vec3 aabbMin = m1 - varClipGamma * sigma;
    vec3 aabbMax = m1 + varClipGamma * sigma;

    // Convert history to YCoCg and clip
    vec3 historyYCoCg = RGBToYCoCg(historyColor);
    historyYCoCg = clipToAABB(aabbMin, aabbMax, historyYCoCg);
    historyColor = YCoCgToRGB(historyYCoCg);

    // Calculate blend factor based on motion
    float velocityMag = length(velocity);
    float motionWeight = clamp(velocityMag * params.motionScale, 0.0, 1.0);

    // Higher motion = trust current frame more
    float blend = mix(params.blendFactor, 0.5, motionWeight);

    // Blend current and history
    vec3 result = mix(currentColor, historyColor, blend);

    outColor = vec4(result, 1.0);
}
