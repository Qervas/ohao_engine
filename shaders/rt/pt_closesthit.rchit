#version 460
#extension GL_EXT_ray_tracing : require

// Path Tracer Closest Hit Shader
// Returns hit position, normal, and surface albedo from material buffer.

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
hitAttributeEXT vec2 baryCoord;

// Material buffer — per-instance (rgb=albedo, a=roughness)
layout(set = 0, binding = 3) readonly buffer MaterialBuffer {
    vec4 materials[];
} materialBuf;

void main() {
    // Compute hit position
    payload.hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    payload.hitDist = gl_HitTEXT;
    payload.hitInstance = gl_InstanceCustomIndexEXT;

    // Compute geometric normal from triangle vertices
    // We use the object-to-world transform to get the world-space normal
    // For now, use the geometric normal from the hit
    // The barycentric coordinates are in baryCoord.xy, with z = 1 - x - y

    // Approximate face normal from ray direction and object transform
    // For axis-aligned boxes (Cornell box), the geometric normal = closest axis
    vec3 hitLocal = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * gl_HitTEXT;

    // Compute face normal by finding which face of the unit cube was hit
    vec3 absLocal = abs(hitLocal);
    vec3 localNormal;
    if (absLocal.x > absLocal.y && absLocal.x > absLocal.z) {
        localNormal = vec3(sign(hitLocal.x), 0, 0);
    } else if (absLocal.y > absLocal.z) {
        localNormal = vec3(0, sign(hitLocal.y), 0);
    } else {
        localNormal = vec3(0, 0, sign(hitLocal.z));
    }

    // Transform normal to world space
    vec3 worldNormal = normalize(mat3(gl_ObjectToWorldEXT) * localNormal);

    // Always face the incoming ray (handle inside-out geometry like Cornell box walls)
    if (dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0) {
        worldNormal = -worldNormal;
    }
    payload.hitNormal = worldNormal;

    // Look up albedo from material buffer
    payload.hitAlbedo = materialBuf.materials[gl_InstanceCustomIndexEXT].rgb;
}
