#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

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
        payload.color = vec3(0.0);
    }
}
