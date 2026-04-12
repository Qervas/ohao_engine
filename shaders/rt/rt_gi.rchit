#version 460
#extension GL_EXT_ray_tracing : require

// Closest Hit Shader for GI — returns the hit surface's albedo + distance
// Uses instance custom index to look up per-surface albedo from material buffer.

layout(location = 0) rayPayloadInEXT vec4 giPayload;
hitAttributeEXT vec2 hitBarycentric;

// Material buffer — one vec4 per instance (rgb=albedo, a=unused)
layout(set = 0, binding = 6) readonly buffer MaterialBuffer {
    vec4 materials[];
} materialBuf;

void main() {
    // Look up albedo from material buffer using instance index
    vec3 albedo = materialBuf.materials[gl_InstanceCustomIndexEXT].rgb;
    giPayload = vec4(albedo, gl_HitTEXT);
}
