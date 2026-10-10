#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

#include "includes/scene/scene_data.glsl"

// Closest Hit Shader for GI — returns the hit surface's albedo + distance.
// The instance custom index is the RenderScene slot.

layout(location = 0) rayPayloadInEXT vec4 giPayload;
hitAttributeEXT vec2 hitBarycentric;

layout(set = 0, binding = 6, std430) readonly buffer Instances { InstanceGpu instances[]; };

void main() {
    // LEGACY rgb = the actor's albedo (InstanceGpu::giAlbedoPacked). Its .a is not
    // read: the old per-instance GI buffer always held alpha 1.0 (static), so every
    // hit counted.
    vec3 albedo = instances[gl_InstanceCustomIndexEXT].giAlbedoPacked.rgb;
    giPayload = vec4(albedo, gl_HitTEXT);
}
