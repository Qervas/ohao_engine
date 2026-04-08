#version 450
#extension GL_ARB_separate_shader_objects : enable

// Water fragment shader — Gerstner waves, PBR Fresnel, IBL+SSR reflections,
// refraction (scene color), shore foam, wave-crest whitecaps, sun specular,
// SSS wave tips, foam texture detail, GPU ripple normal, edge fade.

layout(location = 0) in  vec3  inWorldPos;
layout(location = 1) in  vec3  inNormal;
layout(location = 2) in  vec2  inUV;
layout(location = 3) in  float inFoamFactor;   // wave crest height [0,1]
layout(location = 4) in  float inEdgeFade;     // horizon fade [0,1]

layout(location = 0) out vec4 outColor;

// Descriptor set 0
layout(set = 0, binding = 0) uniform sampler2D  sceneDepth;    // GBuffer depth
layout(set = 0, binding = 1) uniform sampler2D  normalMap1;    // scrolling detail normal 1
layout(set = 0, binding = 2) uniform sampler2D  normalMap2;    // scrolling detail normal 2
layout(set = 0, binding = 3) uniform samplerCube envMap;       // IBL prefiltered env cube
layout(set = 0, binding = 4) uniform sampler2D  brdfLUT;       // IBL BRDF split-sum LUT
layout(set = 0, binding = 5) uniform sampler2D  sceneColor;    // HDR scene color (refraction)
layout(set = 0, binding = 6) uniform sampler2D  ssrOutput;     // Screen-space reflections
layout(set = 0, binding = 7) uniform sampler2D  foamTex;       // Animated bubble/foam noise
layout(set = 0, binding = 8) uniform sampler2D  rippleMap;     // GPU ripple height map (R16F)

layout(push_constant) uniform WaterPC {
    mat4  viewProj;     // 64
    mat4  invViewProj;  // 64
    vec4  cameraPos;    // 16 — xyz=position, w=waterLevel
    vec4  waterParams;  // 16 — x=size, y=time, z=waveAmp, w=foamIntensity
    vec4  sunParams;    // 16 — xyz=sunDir (world-space), w=sunIntensity
    vec4  shallowColor; // 16 — rgb=shallow water color, a=sssStrength
    vec4  deepColor;    // 16 — rgb=deep water color,    a=rippleNormalStrength
} pc;                   // Total: 208 bytes

// Reconstruct world-space position from screen UV + depth.
vec3 reconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc   = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = pc.invViewProj * ndc;
    return world.xyz / world.w;
}

void main() {
    float time          = pc.waterParams.y;
    float foamIntensity = pc.waterParams.w;
    float waveAmp       = pc.waterParams.z;
    float waterSize     = pc.waterParams.x;
    float sssStrength   = pc.shallowColor.a;           // packed: was unused .a
    float rippleNStr    = pc.deepColor.a;               // packed: was unused .a

    // Screen-space UV for sampling depth / scene color / SSR
    ivec2 screenSz = textureSize(sceneDepth, 0);
    vec2  screenUV = gl_FragCoord.xy / vec2(screenSz);

    // ─── Shore depth & foam ───────────────────────────────────────────────
    float sceneDepthVal = texture(sceneDepth, screenUV).r;
    vec3  scenePos      = reconstructWorldPos(screenUV, sceneDepthVal);
    // depthDiff > 0 → geometry above water (shore). depthDiff < 0 → submerged.
    float depthDiff     = scenePos.y - inWorldPos.y;
    // Foam within 0.5 m of the shoreline
    float shoreFoam     = clamp(1.0 - depthDiff / 0.5, 0.0, 1.0) * foamIntensity;
    // Whitecap foam at wave crests (inFoamFactor from vertex)
    float crestFoam     = smoothstep(0.6, 1.0, inFoamFactor) * 0.8;
    float foam          = clamp(shoreFoam + crestFoam, 0.0, 1.0);

    // ─── Detail normal maps ───────────────────────────────────────────────
    vec2 uv1 = inUV * 4.0 + vec2( time * 0.020,  time * 0.010);
    vec2 uv2 = inUV * 3.0 + vec2(-time * 0.015,  time * 0.025);
    vec3 n1  = texture(normalMap1, uv1).rgb * 2.0 - 1.0;
    vec3 n2  = texture(normalMap2, uv2).rgb * 2.0 - 1.0;
    // Perturb Gerstner normal with detail maps (0.3 strength)
    vec3 N   = normalize(inNormal + (n1 + n2) * 0.3);

    // ─── GPU ripple normal perturbation ───────────────────────────────────
    // Sample ripple height map and compute gradient via finite differences
    if (rippleNStr > 0.001) {
        vec2 rippleUV = inWorldPos.xz / waterSize + 0.5;
        vec2 texelSz  = 1.0 / vec2(256.0);
        float rC = texture(rippleMap, rippleUV).r;
        float rE = texture(rippleMap, rippleUV + vec2(texelSz.x, 0.0)).r;
        float rN = texture(rippleMap, rippleUV + vec2(0.0, texelSz.y)).r;
        vec3 rippleN = normalize(vec3(rC - rE, 1.0 / rippleNStr, rC - rN));
        N = normalize(N + rippleN * rippleNStr);
    }

    // ─── View & reflection ────────────────────────────────────────────────
    vec3  V      = normalize(pc.cameraPos.xyz - inWorldPos);
    float NdotV  = max(dot(N, V), 0.0);
    vec3  R      = reflect(-V, N);

    // ─── Fresnel (Schlick, water F0 = 0.02, IOR ≈ 1.33) ──────────────────
    float fresnel = mix(0.02, 1.0, pow(1.0 - NdotV, 4.0));

    // ─── IBL specular ─────────────────────────────────────────────────────
    float roughness   = 0.04 + foam * 0.2;  // foam makes surface rougher
    float mipLevel    = roughness * 6.0;
    vec3  envColor    = textureLod(envMap, R, mipLevel).rgb;
    vec2  brdfUV      = clamp(vec2(NdotV, roughness), vec2(0.0), vec2(1.0));
    vec2  brdfSplit   = texture(brdfLUT, brdfUV).rg;
    vec3  specularIBL = envColor * (0.02 * brdfSplit.x + brdfSplit.y);

    // ─── SSR blend (optional — blends over IBL if SSR has signal) ─────────
    vec4  ssrSample   = texture(ssrOutput, screenUV);
    vec3  reflection  = mix(specularIBL, ssrSample.rgb, ssrSample.a * 0.8);

    // ─── Sun specular (Blinn-Phong, physically scaled by Fresnel) ─────────
    vec3  L         = normalize(pc.sunParams.xyz);
    vec3  H         = normalize(V + L);
    float NdotH     = max(dot(N, H), 0.0);
    float sunSpec   = pow(NdotH, 512.0) * fresnel * pc.sunParams.w;
    sunSpec        *= max(dot(vec3(0.0, 1.0, 0.0), L), 0.0);

    // ─── Sub-surface scattering at wave tips ──────────────────────────────
    // Back-lit glow: when light comes from behind the wave, crests transmit
    // a teal/green colour (like thin ocean waves in real life).
    float backlit  = max(dot(-L, N), 0.0);
    float sss      = pow(backlit, 2.5) * inFoamFactor * sssStrength;
    vec3  sssColor = vec3(0.08, 0.65, 0.45) * sss * pc.sunParams.w;

    // ─── Refraction (scene color distorted by normal) ─────────────────────
    vec2  refrOffset  = N.xz * 0.03 * clamp(-depthDiff * 0.5, 0.0, 1.0);
    vec3  refracted   = texture(sceneColor, screenUV + refrOffset).rgb;

    // ─── Depth-based water color ──────────────────────────────────────────
    float depthFactor  = clamp(-depthDiff * 0.15, 0.0, 1.0);
    vec3  waterColor   = mix(pc.shallowColor.rgb, pc.deepColor.rgb, depthFactor);
    vec3  refractColor = mix(refracted, waterColor, clamp(-depthDiff * 0.3, 0.0, 0.85));

    // ─── Foam texture detail ──────────────────────────────────────────────
    // Two-scale animated bubble/foam noise modulates the flat foam color
    // Primary UV uses parallax offset from surface normal to add depth illusion
    vec2 foamUV1   = inUV * 8.0 + vec2(time * 0.04, time * 0.035);
    foamUV1       += N.xz * inFoamFactor * 0.04;  // parallax offset
    vec2 foamUV2   = inUV * 3.0 - vec2(time * 0.03, time * 0.05);
    float foamNoise = texture(foamTex, foamUV1).r * 0.6
                    + texture(foamTex, foamUV2).r * 0.4;
    float detailFoam = foam * (foamNoise * 0.7 + 0.3);

    // ─── Final composite ──────────────────────────────────────────────────
    vec3 color = mix(refractColor, reflection, fresnel);
    color += vec3(sunSpec);
    color += sssColor;
    // Foam texture replaces flat white foam
    color = mix(color, vec3(1.0, 0.98, 0.95), detailFoam);

    // Alpha: grazing angles look fully opaque (reflected), head-on more transparent.
    // Edge fade: alpha → 0 at grid boundary so infinite ocean has no visible edge.
    float alpha = clamp(0.75 + fresnel * 0.25, 0.0, 1.0) * inEdgeFade;

    outColor = vec4(color, alpha);
}
