#version 450
#extension GL_GOOGLE_include_directive : enable

// Deferred Lighting Fragment Shader
// Reads G-Buffer and evaluates all lights in a single pass.
// CSM: selects cascade based on view-space depth, samples shadow map array.

#include "includes/common/encoding.glsl"
#include "includes/brdf/brdf_ggx.glsl"
#include "shared/layout.glsl"

layout(location = 0) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;
// For SSR's resolve (postprocess/ssr_resolve.slang), which replaces the environment's reflection
// where the screen-space trace hits: E, the environment/RT GI specular this pixel added (rgb),
// and W, its weight, the split sum F0 * A + B times specular occlusion (rgb). 0 for sky.
layout(location = 1) out vec4 outEnvSpecular;
layout(location = 2) out vec4 outSpecularWeight;

// G-Buffer samplers (shader uses bindings 0-2; C++ has 5 gbuffer bindings but 3,4 unused here)
layout(set = 0, binding = 0) uniform sampler2D gBuffer0; // Position + Metallic
layout(set = 0, binding = 1) uniform sampler2D gBuffer1; // Normal + Roughness
layout(set = 0, binding = 2) uniform sampler2D gBuffer2; // Albedo + AO
layout(set = 0, binding = 17) uniform sampler2D gBufferEmissive; // emitted radiance (rgb), core/gbuffer.frag

// The light table (binding 5, shared/layout.json's LightGpu), and the froxels' light lists
// (binding 16, compute/light_cluster.slang: kClusterCount counts, then kClusterMaxLights light
// indices a froxel, in light order).
layout(set = 0, binding = 5, std430) readonly buffer Lights { LightGpu lights[]; };
layout(set = 0, binding = 16, std430) readonly buffer Clusters { uint clusterData[]; };

// Shadow map — CSM array (4 cascades), through the pass's comparison sampler (LESS_OR_EQUAL):
// a lookup returns 1 where the reference depth is at or in front of the stored one.
layout(set = 0, binding = 6) uniform sampler2DArrayShadow shadowMap;

// Image-based lighting (render/deferred/env_prefilter, shaders/ibl/env_prefilter.slang), both
// equirectangular: the environment's irradiance over pi (binding 7) and the environment GGX-
// prefiltered by roughness, level i at roughness i / (kEnvPrefilterLevels - 1) (binding 15).
layout(set = 0, binding = 7) uniform sampler2D envIrradiance;
layout(set = 0, binding = 15) uniform sampler2D envPrefiltered;

// SSAO (binding 10, flag bit 1): half resolution, (ao, distance from the camera; 0 for sky),
// written by compute/ssao.slang; sample q sits on full-resolution pixel 2q.
layout(set = 0, binding = 10) uniform sampler2D ssaoTexture;

// RT GI (binding 11), full resolution: rgb the mean incoming radiance over the cosine lobe (the
// diffuse irradiance over pi, the environment through the rays that escaped, the room's bounce
// light through those that hit), a the environment's visibility (the share that escaped).
layout(set = 0, binding = 11) uniform sampler2D ssgiTexture;

// Cascade shadow map data (binding 12)
layout(set = 0, binding = 12) uniform CascadeUBO {
    mat4 viewProj[4];
    vec4 splitDepths;       // positive view-space distances per cascade
    float cascadeBlendWidth;
    float shadowBias;
    float normalBias;
    uint  cascadeCount;
} cascades;

// RT shadow visibility (binding 14): one 8-bit channel per shadow light (a light's params.w),
// 0 shadowed, 255 lit, in an RG32UI image (x: lights 0-3, y: 4-7, lowest byte first), the
// lighting pass's size. Written by rt/shadow_filter.slang (realtime) or rt/shadow_trace.slang
// (reference).
layout(set = 0, binding = 14) uniform usampler2D rtShadowVisibility;

// Push constants (matches C++ LightingParams — 184 bytes, under NVIDIA 256-byte limit)
layout(push_constant) uniform PushConstants {
    mat4 invViewProj;
    mat4 view;           // for cascade depth selection and the froxel's slice
    vec3 cameraPos;
    float ambientIntensity;
    vec2 screenSize;
    uint lightCount;     // the light table's (every light, without clusters)
    uint flags;          // Bit 0: IBL, Bit 1: SSAO, Bit 2: CSM shadows, Bit 3: SSGI, Bit 5: RT shadows, Bit 6: every light (no clusters)
    float wetness;       // 0=dry, 1=fully wet — drives surface material modulation
    float sliceScale;    // the froxel slice: floor(log(distance) * sliceScale + sliceBias) (ClusterPush.screen.zw)
    float snowCover;     // 0=bare, 1=fully snow-covered — white matte coating on flat faces
    float sliceBias;
    float frostCover;    // 0=bare, 1=fully frost/ice coated
    float envIntensity;  // Environment.intensity: scales the prefiltered environment and its irradiance
} pc;                   // Total: 184 bytes

// BRDF functions provided by #include "includes/brdf/brdf_ggx.glsl"
// Using height-correlated Smith-GGX (Frostbite/Unity quality)

// Calculate attenuation for point/spot lights
float calculateAttenuation(vec3 lightPos, vec3 fragPos, float range) {
    float distance = length(lightPos - fragPos);
    // Physically-correct inverse-square falloff with smooth range cutoff
    // Matches Blender/UE5 light attenuation
    float d2 = distance * distance;
    float invSq = 1.0 / max(d2, 0.01);  // inverse-square (physical)
    float windowing = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);
    return invSq * windowing * windowing;
}

// Spot light cone attenuation
float calculateSpotAttenuation(vec3 L, vec3 spotDir, float innerCone, float outerCone) {
    float theta = dot(L, -spotDir);
    return clamp((theta - outerCone) / (innerCone - outerCone), 0.0, 1.0);
}

// SSAO at this pixel: the 4 nearest half-resolution samples, weighted bilinearly and by how
// close their distance from the camera is to this pixel's (within 5%); where none is the same
// surface, the closest one.
float ssaoAt(ivec2 pixel, float dist) {
    const ivec2 size = textureSize(ssaoTexture, 0);
    const ivec2 base = pixel >> 1;
    const vec2 f = vec2(pixel & 1) * 0.5;
    float sum = 0.0, wsum = 0.0, best = 1.0, bestErr = 1e30;
    for (int i = 0; i < 4; ++i) {
        const ivec2 o = ivec2(i & 1, i >> 1);
        const vec2 s = texelFetch(ssaoTexture, min(base + o, size - 1), 0).xy;
        if (s.y <= 0.0) continue;
        const float err = abs(s.y - dist);
        if (err < bestErr) { bestErr = err; best = s.x; }
        const float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y) *
                        max(0.0, 1.0 - err / (0.05 * dist));
        sum += w * s.x;
        wsum += w;
    }
    return wsum > 1e-3 ? sum / wsum : best;
}

// A direction's texel coordinate in the equirectangular environment images.
vec2 equirectUV(vec3 d) {
    return vec2(atan(d.z, d.x) / 6.2831853 + 0.5, asin(clamp(d.y, -1.0, 1.0)) / 3.1415926 + 0.5);
}

// How much of the environment's specular reflection survives the occlusion `ao` of the
// diffuse terms (Lagarde and de Rousiers 2014, "Moving Frostbite to PBR"): the reflection
// cone narrows with smoothness and at grazing angles, so it is occluded less than the
// cosine-weighted hemisphere ao measures, and fully at ao 0.
float specularOcclusion(float NdotV, float ao, float roughness) {
    return clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

// Select CSM cascade based on positive view-space depth
int selectCascade(float viewDepth) {
    uint count = min(cascades.cascadeCount, 4u);
    for (int i = 0; i < int(count); ++i) {
        if (viewDepth < cascades.splitDepths[i]) return i;
    }
    return int(count) - 1;
}

// PCF shadow calculation using CSM array
float calculateShadowCSM(vec3 fragPos, float viewDepth) {
    int cascadeIdx = selectCascade(viewDepth);
    mat4 lightSpaceMatrix = cascades.viewProj[cascadeIdx];

    vec4 fragPosLightSpace = lightSpaceMatrix * vec4(fragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    // NDC [-1,1] -> UV [0,1] for X and Y
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Discard fragments outside the light frustum
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0) {
        return 1.0;
    }

    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float reference = projCoords.z - cascades.shadowBias;

    // 3x3 PCF, each tap a hardware comparison
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            shadow += texture(shadowMap, vec4(projCoords.xy + vec2(x, y) * texelSize, float(cascadeIdx), reference));
        }
    }

    return shadow / 9.0;
}

void main() {
    // Sample G-Buffer
    vec4 gBuffer0Sample = texture(gBuffer0, inTexCoord);
    vec4 gBuffer1Sample = texture(gBuffer1, inTexCoord);
    vec4 gBuffer2Sample = texture(gBuffer2, inTexCoord);

    // Early discard for sky/background — position (0,0,0) means no geometry was written
    if (gBuffer0Sample.rgb == vec3(0.0) && gBuffer0Sample.a == 0.0) {
        outColor = vec4(0.0);
        outEnvSpecular = vec4(0.0);
        outSpecularWeight = vec4(0.0);
        return;
    }

    // A light's emitter mesh (emissive alpha 1, core/gbuffer.frag): its radiance and nothing
    // else. No light, environment or GI reaches it, and with E = W = 0 SSR leaves it alone.
    const vec4 emitted = texture(gBufferEmissive, inTexCoord);
    if (emitted.a > 0.5) {
        outColor = vec4(emitted.rgb, 1.0);
        outEnvSpecular = vec4(0.0);
        outSpecularWeight = vec4(0.0);
        return;
    }

    // Unpack G-Buffer
    vec3 fragPos = gBuffer0Sample.rgb;
    float metallic = gBuffer0Sample.a;

    vec3 N = decodeNormalOctahedron(gBuffer1Sample.xy);
    float roughness = gBuffer1Sample.b;

    vec3 albedo = gBuffer2Sample.rgb;
    float ao = gBuffer2Sample.a;

    // Ground wetness — modulate material based on rain accumulation.
    // Slope factor: horizontal faces (N.y near 1) accumulate water; vertical walls don't.
    if (pc.wetness > 0.001) {
        float slopeFactor = clamp(N.y * 2.0, 0.0, 1.0);
        float w = pc.wetness * slopeFactor;
        albedo    *= 1.0 - w * 0.30;
        roughness  = mix(roughness, max(roughness * 0.15, 0.02), w);
    }

    // Snow accumulation — white matte coating on upward-facing surfaces.
    // Steeper cutoff than wetness: snow only sticks to surfaces above ~17° from horizontal.
    if (pc.snowCover > 0.001) {
        float slopeFactor = clamp((N.y - 0.3) * (1.0 / 0.7), 0.0, 1.0);
        float s = pc.snowCover * slopeFactor;
        albedo    = mix(albedo,    vec3(0.82, 0.87, 0.92), s); // white-ish snow tint
        roughness = mix(roughness, 0.90,                    s); // snow is matte
        metallic  = mix(metallic,  0.0,                     s); // snow is non-metallic
    }

    // Mud — high wetness on non-metallic surfaces develops an earth-tone tint.
    // Only appears at wetness > 0.5 to avoid tinting lightly-wet surfaces.
    if (pc.wetness > 0.5) {
        float slopeFactor = clamp(N.y * 2.0, 0.0, 1.0);
        float w = pc.wetness * slopeFactor;
        float mud = clamp((w - 0.5) * 2.0, 0.0, 1.0) * (1.0 - metallic);
        const vec3 mudTint = vec3(0.35, 0.27, 0.18);  // dark earth
        albedo = mix(albedo, albedo * mudTint, mud * 0.55);
    }

    // Frost / Ice — forms when snow accumulation is high; crystalline coating.
    if (pc.frostCover > 0.001) {
        float f = pc.frostCover;
        albedo    = mix(albedo,    vec3(0.78, 0.88, 0.95), f * 0.50);  // pale ice blue
        roughness = mix(roughness, 0.08,                    f * 0.70);  // ice = very smooth
        metallic  = mix(metallic,  0.10,                    f * 0.40);  // slight reflectance
    }

    // View direction — use push constant camera position
    vec3 V = normalize(pc.cameraPos - fragPos);

    // View-space depth for cascade selection (positive distance from camera)
    vec4 fragPosView = pc.view * vec4(fragPos, 1.0);
    float viewDepth = -fragPosView.z; // negate: right-handed view space, z is negative in front

    // Build BRDF surface once (used for all lights)
    BRDFSurface surface = initBRDFSurface(fragPos, N, V, albedo, metallic, roughness, ao);

    // Accumulate lighting from SSBO data
    vec3 Lo = vec3(0.0);

    // The lights that can reach this pixel: its froxel's list (in light order), or with flag
    // bit 6 every light. A light left out contributes exactly zero here (past its range), and
    // so does one skipped below (no attenuation, fully shadowed): the sum is the same either way.
    uint lightCount = pc.lightCount;
    uint listBase = 0u;
    const bool clustered = (pc.flags & 64u) == 0u;
    if (clustered) {
        const uint tx = min(uint(gl_FragCoord.x * float(kClusterTilesX) / pc.screenSize.x), kClusterTilesX - 1u);
        const uint ty = min(uint(gl_FragCoord.y * float(kClusterTilesY) / pc.screenSize.y), kClusterTilesY - 1u);
        const float s = viewDepth > 0.0 ? floor(log(viewDepth) * pc.sliceScale + pc.sliceBias) : 0.0;
        const uint slice = uint(clamp(s, 0.0, float(kClusterSlices - 1u)));
        const uint cluster = (slice * kClusterTilesY + ty) * kClusterTilesX + tx;
        lightCount = clusterData[cluster];
        listBase = kClusterCount + cluster * kClusterMaxLights;
    }
    const bool rtShadows = (pc.flags & 32u) != 0u;
    const uvec2 rtShadow = rtShadows ? texelFetch(rtShadowVisibility, ivec2(gl_FragCoord.xy), 0).xy : uvec2(0u);
    for (uint k = 0u; k < lightCount; ++k) {
        const LightGpu light = lights[clustered ? clusterData[listBase + k] : k];
        int lightType = int(light.position.w);

        vec3 L;
        float attenuation = 1.0;

        if (lightType == 0) {
            L = normalize(-light.direction.xyz);
        } else if (lightType == 1) {
            vec3 lightPos = light.position.xyz;
            L = normalize(lightPos - fragPos);
            attenuation = calculateAttenuation(lightPos, fragPos, light.direction.w);
        } else if (lightType == 2) {
            vec3 lightPos = light.position.xyz;
            L = normalize(lightPos - fragPos);
            attenuation = calculateAttenuation(lightPos, fragPos, light.direction.w);
            attenuation *= calculateSpotAttenuation(L, normalize(light.direction.xyz),
                                                     light.params.x, light.params.y);
        } else {
            // A rectangle (type 3): its radiance, colour times color.w = I / (4 pi r^2), lights by
            // its own irradiance below; no range window and no 1/d^2, as the path tracer's.
            L = normalize(light.position.xyz - fragPos);
        }

        if (attenuation == 0.0) continue;   // adds exactly zero: past its range or its cone
        vec3 radiance = light.color.rgb * light.color.w * attenuation;

        // Shadow determination — RT shadows (bit 5) override CSM (bit 2). A light that
        // casts no shadows (slot -1) is never occluded; without RT shadows a directional
        // one gets CSM.
        float shadow = 1.0;
        int slot = int(round(light.params.w));
        if (slot >= 0) {
            if (rtShadows) {
                const uint word = slot < 4 ? rtShadow.x : rtShadow.y;
                shadow = float((word >> (8u * uint(slot & 3))) & 255u) * (1.0 / 255.0);
            } else if ((pc.flags & 4u) != 0u && lightType == 0) {
                shadow = calculateShadowCSM(fragPos, viewDepth);
            }
        }
        if (shadow == 0.0) continue;   // fully occluded: adds exactly zero

        // Cook-Torrance BRDF via include (height-correlated Smith-GGX). A point or spot light with
        // an emitter mesh (params.z: its sphere's radius) reflects as that sphere: a mirror shows
        // the mesh, at its radiance, wherever it is (on screen or not, SSR or not).
        if (lightType == 3)
            Lo += evaluateBRDFRect(surface, fragPos, light.position.xyz, light.direction.xyz, light.params.xyz, radiance) * shadow;
        else if (lightType != 0 && light.params.z > 0.0)
            Lo += evaluateBRDFSphere(surface, light.position.xyz - fragPos, light.params.z, radiance) * shadow;
        else
            Lo += evaluateBRDF(surface, L, radiance) * shadow;
    }

    // SSAO (flag bit 1) darkens the ambient and environment terms only: not direct light,
    // which RT shadows or CSM occlude, and not RT GI, whose rays already see occluders.
    const float ssao = (pc.flags & 2u) != 0u ? ssaoAt(ivec2(gl_FragCoord.xy), distance(fragPos, pc.cameraPos)) : 1.0;

    // Indirect diffuse light comes from RT GI's rays (flag bit 3, rt/gi_trace.slang and
    // gi_denoise.slang: the incoming radiance, times the diffuse albedo here) or, without them,
    // from the environment's irradiance (the IBL block below). Only a frame with neither falls
    // back to the constant ambient, a fill with no counterpart in the path tracer. All three are
    // diffuse: the diffuse albedo only (a metal has none; its albedo is its F0, and the
    // reflection below is all the light it returns).
    const bool rtGI = (pc.flags & 8u) != 0u;
    const bool ibl = (pc.flags & 1u) != 0u;
    const vec4 gi = rtGI ? texture(ssgiTexture, inTexCoord) : vec4(0.0, 0.0, 0.0, 1.0);
    vec3 ambient = vec3(0.0);
    if (rtGI) ambient += gi.rgb * (albedo * (1.0 - metallic)) * ao;
    else if (!ibl) ambient += vec3(pc.ambientIntensity) * albedo * (1.0 - metallic) * ao * ssao;

    // The reflection's weight, for the environment here and for SSR's resolve: the GGX BRDF's
    // directional albedo F0 * A + B (Karis's analytic fit, envBRDFApprox) times the share of the
    // reflection that survives the occlusion below what RT GI's rays resolve, ao (the
    // material's) and SSAO, through specularOcclusion (the reflection is a narrower cone than
    // the hemisphere they measure).
    const float NdotV = max(dot(N, V), 1e-4);
    const vec2 ab = envBRDFApprox(NdotV, roughness);
    const float occlusion = ao * ssao;
    const vec3 specularWeight = (surface.F0 * ab.x + ab.y) * specularOcclusion(NdotV, occlusion, roughness);
    vec3 envSpecular = vec3(0.0);

    // Image-based lighting (flag bit 0: an environment is loaded and prefiltered). The split
    // sum: the radiance arriving around the reflection at this roughness, times the weight.
    if (ibl) {
        const vec3 R = reflect(-V, N);
        const vec3 prefiltered = pc.envIntensity *
            textureLod(envPrefiltered, equirectUV(R), roughness * float(kEnvPrefilterLevels - 1u)).rgb;
        const vec3 irradiance = pc.envIntensity * textureLod(envIrradiance, equirectUV(N), 0.0).rgb;   // over pi
        // What arrives: the environment where it is visible, and with RT GI, what the rays that
        // hit something brought back (the incoming radiance less the escaped rays' share,
        // which is the visibility times the environment's own mean) in place of the rest.
        // Without RT GI the environment is taken as unoccluded (visibility 1).
        const vec3 incoming = prefiltered * gi.a + (rtGI ? max(gi.rgb - irradiance * gi.a, vec3(0.0)) : vec3(0.0));
        envSpecular = incoming * specularWeight;
        ambient += envSpecular;

        // RT GI's rays that leave the scene bring back the environment, occluded: with it, the
        // unoccluded diffuse term here would count that light twice.
        if (!rtGI) {
            const vec3 kD = (1.0 - fresnelSchlickRoughness(NdotV, surface.F0, roughness)) * (1.0 - metallic);
            ambient += kD * albedo * irradiance * occlusion;
        }
    }

    // Emitted radiance, as the G-buffer wrote it (the emissive texture times its strength, the
    // path tracer's hit emission): its own colour, independent of the albedo and the lighting
    const vec3 emissive = emitted.rgb;

    // Final color
    vec3 color = ambient + Lo + emissive;


    outColor = vec4(color, 1.0);
    outEnvSpecular = vec4(envSpecular, 0.0);
    outSpecularWeight = vec4(specularWeight, 0.0);
}
