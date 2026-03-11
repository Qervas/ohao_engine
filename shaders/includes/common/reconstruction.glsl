// reconstruction.glsl — View/world-space position reconstruction from depth
// Shared across: ssao, ssgi, ssr, volumetric_scatter, water.frag
//
// Part of OHAO Engine shader system
// Location: includes/common/reconstruction.glsl

#ifndef OHAO_COMMON_RECONSTRUCTION_GLSL
#define OHAO_COMMON_RECONSTRUCTION_GLSL

// ---------------------------------------------------------------------------
// Reconstruct view-space position from UV + depth + inverse projection
// Used by: ssao, ssr
// ---------------------------------------------------------------------------
vec3 reconstructViewPos(vec2 uv, float depth, mat4 invProjection) {
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 viewPos = invProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

// ---------------------------------------------------------------------------
// Reconstruct world-space position from UV + linear depth + inv matrices
// Used by: volumetric_scatter
// ---------------------------------------------------------------------------
vec3 reconstructWorldPos(vec2 uv, float linearDepth, mat4 invProjection, mat4 invView) {
    vec4 clipPos = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vec4 viewPos = invProjection * clipPos;
    viewPos /= viewPos.w;
    vec3 viewDir = normalize(viewPos.xyz);
    vec3 worldDir = mat3(invView) * viewDir;
    vec3 camPos = invView[3].xyz;
    return camPos + worldDir * linearDepth;
}

// ---------------------------------------------------------------------------
// Reconstruct world-space direction from UV + inverse view-projection
// Used by: cloud.comp, water.frag (ray origin/direction from screen pixel)
// ---------------------------------------------------------------------------
vec3 reconstructWorldDir(vec2 uv, mat4 invViewProj, vec3 cameraPos) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 worldFar = invViewProj * vec4(ndc, 1.0, 1.0);
    worldFar /= worldFar.w;
    return normalize(worldFar.xyz - cameraPos);
}

#endif // OHAO_COMMON_RECONSTRUCTION_GLSL
