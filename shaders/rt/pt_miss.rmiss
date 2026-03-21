#version 460
#extension GL_EXT_ray_tracing : require

// Path Tracer Miss Shader — ray escaped the scene
// For enclosed scenes (Cornell box), miss = black (no contribution)

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

void main() {
    payload.hitDist = -1.0;  // signal miss
    payload.color = vec3(0.0);
}
