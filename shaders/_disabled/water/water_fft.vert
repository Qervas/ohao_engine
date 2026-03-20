#version 450
#extension GL_ARB_separate_shader_objects : enable

// Water vertex shader — FFT ocean displacement.
// Identical WaterPC as water.vert (208 bytes) so the same pipeline layout is shared.
// Wave displacement comes from pre-computed FFT textures (bindings 9 + 10)
// rather than inline Gerstner math.  water.frag is shared unchanged.

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

// FFT displacement map (post-IFFT hktImg):
//   .r = height Y displacement   .b = choppiness Dx displacement
layout(set = 0, binding = 9)  uniform sampler2D displacementMap;

// FFT normal map (from fft_normal.comp):
//   .rgb = packed surface normal [0,1]   .a = foam factor
layout(set = 0, binding = 10) uniform sampler2D fftNormalMap;

layout(location = 0) out vec3  outWorldPos;
layout(location = 1) out vec3  outNormal;
layout(location = 2) out vec2  outUV;
layout(location = 3) out float outFoamFactor;
layout(location = 4) out float outEdgeFade;

void main() {
    float waterSize = pc.waterParams.x;
    float waterY    = pc.cameraPos.w;
    float waveAmp   = pc.waterParams.z;

    // Camera-relative infinite grid — same as Gerstner variant
    vec3 basePos = vec3(
        pc.cameraPos.x + inXZ.x * waterSize,
        waterY,
        pc.cameraPos.z + inXZ.y * waterSize
    );

    // UV in [0, 1] for texture sampling
    vec2 uv = inXZ + 0.5;

    // Sample FFT displacement (artist-controlled scale via waveAmp)
    vec4 dispSample = texture(displacementMap, uv);
    float dispY = dispSample.r * waveAmp;  // vertical
    float dispX = dispSample.b * waveAmp;  // horizontal choppiness (X)

    vec3 worldPos = basePos + vec3(dispX, dispY, 0.0);

    // Sample pre-computed FFT normal and foam
    vec4  fftNorm = texture(fftNormalMap, uv);
    vec3  normal  = fftNorm.rgb * 2.0 - 1.0;  // unpack [0,1] → [-1,1]

    outWorldPos   = worldPos;
    outNormal     = normalize(normal);
    outUV         = uv;
    outFoamFactor = fftNorm.a;  // jacobian-based foam from fft_normal.comp

    // Edge fade: same formula as Gerstner variant for seamless fallback switching
    outEdgeFade = 1.0 - smoothstep(0.4, 0.5, max(abs(inXZ.x), abs(inXZ.y)));

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
}
