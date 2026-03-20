#version 450
#extension GL_ARB_separate_shader_objects : enable

// Water vertex shader — Gerstner wave displacement.
// Input is a flat N×N grid of vec2 XZ coordinates in [-0.5, 0.5] normalized space.
// The vertex shader displaces each point according to 4 summed Gerstner waves.

layout(location = 0) in vec2 inXZ;

layout(push_constant) uniform WaterPC {
    mat4  viewProj;     // 64
    mat4  invViewProj;  // 64
    vec4  cameraPos;    // 16 — xyz=position, w=waterLevel
    vec4  waterParams;  // 16 — x=size, y=time, z=waveAmp, w=foamIntensity
    vec4  sunParams;    // 16 — xyz=sunDir, w=sunIntensity
    vec4  shallowColor; // 16 — rgb=shallow, a=sssStrength
    vec4  deepColor;    // 16 — rgb=deep,    a=rippleNormalStrength
} pc;                   // Total: 208 bytes

layout(location = 0) out vec3  outWorldPos;
layout(location = 1) out vec3  outNormal;
layout(location = 2) out vec2  outUV;
layout(location = 3) out float outFoamFactor;
layout(location = 4) out float outEdgeFade;

#include "water/gerstner.glsl"

void main() {
    float waterSize = pc.waterParams.x;
    float time      = pc.waterParams.y;
    float waveAmp   = pc.waterParams.z;
    float waterY    = pc.cameraPos.w;

    // Camera-relative grid: always centered under the camera so the ocean
    // appears infinite — the grid edge is always beyond the visible horizon.
    vec3 basePos = vec3(
        pc.cameraPos.x + inXZ.x * waterSize,
        waterY,
        pc.cameraPos.z + inXZ.y * waterSize
    );

    // 4 Gerstner waves at different directions, wavelengths and steepnesses.
    // The steepness parameter already encodes waveAmp scaling so the artist
    // can scale all waves uniformly via the waterParams.z push constant.
    GerstnerWave waves[4];
    waves[0] = GerstnerWave(vec2( 1.0,  0.0),  8.0, waveAmp * 1.0, 1.0);
    waves[1] = GerstnerWave(vec2( 0.7,  0.7), 12.0, waveAmp * 0.8, 0.8);
    waves[2] = GerstnerWave(vec2(-0.3,  0.9),  5.0, waveAmp * 0.5, 1.2);
    waves[3] = GerstnerWave(vec2( 0.2, -0.8),  3.0, waveAmp * 0.3, 1.5);

    vec3 disp       = vec3(0.0);
    vec3 normAccum  = vec3(0.0, 1.0, 0.0);  // start from geometric up

    for (int i = 0; i < 4; i++) {
        disp      += gerstnerDisplace(basePos, waves[i], time);
        normAccum += gerstnerNormal(basePos, waves[i], time);
    }

    vec3 worldPos = basePos + disp;

    outWorldPos   = worldPos;
    outNormal     = normalize(normAccum);
    outUV         = inXZ + 0.5;  // remap [-0.5, 0.5] → [0, 1]
    // Wave crest factor: how high above base waterLevel this vertex is.
    // Used in frag for whitecap foam at wave peaks.
    float crestRange = pc.waterParams.z + 0.001;  // waveAmp (avoid div by zero)
    outFoamFactor = clamp((worldPos.y - waterY) / crestRange, 0.0, 1.0);

    // Edge fade: smoothly reduce alpha at the grid boundary so the
    // camera-relative ocean fades into the sky rather than showing a hard edge.
    outEdgeFade = 1.0 - smoothstep(0.4, 0.5, max(abs(inXZ.x), abs(inXZ.y)));

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
}
