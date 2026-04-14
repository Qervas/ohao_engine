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
    vec3 albedo = materialBuf.materials[gl_InstanceID].rgb;

    // Workaround for RTX 5070 driver bug: traceRayEXT cull mask is ignored.
    // Detect animated instances by checking material alpha channel.
    // Static walls have alpha=1.0, animated instances have alpha=0.0.
    float isStatic = materialBuf.materials[gl_InstanceID].a;
    if (isStatic < 0.5) {
        giPayload = vec4(0.0, 0.0, 0.0, -1.0);  // treat animated as miss
        return;
    }

    giPayload = vec4(albedo, gl_HitTEXT);
}
