#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

// Path Tracer Miss Shader — ray escaped the scene
// Samples HDR environment map if available, otherwise returns black

struct RayPayload {
    vec3 color;
    vec3 attenuation;
    vec3 hitPos;
    vec3 hitNormal;
    vec3 hitAlbedo;
    float hitDist;
    uint hitInstance;
    float envPdf;       // set by miss shader when env map sampled; used for MIS weighting
};

layout(location = 0) rayPayloadInEXT RayPayload payload;

// Bindless textures (same binding as closest-hit)
layout(set = 0, binding = 12) uniform sampler2D textures[];

// Environment map index passed via light buffer header
layout(set = 0, binding = 11) readonly buffer LightBuffer {
    uint lightCount;
    uint envMapTexIdx;    // bindless index of HDR env map (0xFFFFFFFF = none)
    uint _pad[2];
    // GPULight lights[] follows at offset 16
} lightBuf;

layout(set = 0, binding = 17) readonly buffer EnvMarginalCDF   { float data[]; } envMarg;
layout(set = 0, binding = 18) readonly buffer EnvConditionalCDF { float data[]; } envCond;

layout(push_constant) uniform PushConstants {
    mat4 invView;
    mat4 invProj;
    mat4 prevViewProj;
    uvec4 params;
    uvec4 control;
    vec4 tuning;
    vec4 jitter;                // 4.F T4: matches raygen — needed so the miss
                                // shader's push-constant block size matches the
                                // pipeline layout. Not otherwise read here.
} pc;

#include "includes/rt/env_sampling.glsl"

// Convert ray direction to equirectangular UV
vec2 dirToEquirect(vec3 dir) {
    float phi = atan(dir.z, dir.x);         // [-pi, pi]
    float theta = asin(clamp(dir.y, -1.0, 1.0));  // [-pi/2, pi/2]
    return vec2(phi / 6.2831853 + 0.5, theta / 3.1415926 + 0.5);
}

void main() {
    payload.hitDist = -1.0;  // signal miss

    // Sample environment map if available
    if (lightBuf.envMapTexIdx != 0xFFFFFFFFu) {
        vec3 dir = normalize(gl_WorldRayDirectionEXT);
        vec2 uv = dirToEquirect(dir);
        payload.color = texture(textures[nonuniformEXT(lightBuf.envMapTexIdx)], uv).rgb;
    } else {
        // No env map — return BLACK. A fake "sky ambient" here is physically wrong
        // for closed/indoor scenes: GI rays that escape through wall seams pick it
        // up and carry spurious sky light (+ grain) back into a sealed room. Indoor
        // scenes must be lit only by their own lights.
        payload.color = vec3(0.0);
    }

    // Report env PDF so the caller can apply MIS weighting on BSDF-side env hits.
    if (lightBuf.envMapTexIdx != 0xFFFFFFFFu && pc.control.w > 0u && pc.tuning.y > 0.0) {
        vec3 dir = normalize(gl_WorldRayDirectionEXT);
        payload.envPdf = pdfEnvMap(dir, pc.control.w, uint(pc.tuning.y));
    } else {
        payload.envPdf = 0.0;
    }
}
