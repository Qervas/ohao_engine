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

    // Compute normal in object space
    vec3 hitLocal = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * gl_HitTEXT;

    // Detect if this is a sphere or a box based on vertex count / geometry shape
    // Sphere: normal = normalized local position (points outward from center)
    // Box: normal = closest axis face
    float sphereTest = length(hitLocal);  // for a unit sphere, this ≈ 1.0
    vec3 localNormal;

    // Distinguish sphere vs cube in object space:
    // Cube: at least one component of |hitLocal| ≈ 1.0 (face), others < 1.0
    // Sphere: all components satisfy x²+y²+z² ≈ 1.0
    // Test: for a cube face, max(|xyz|) ≈ 1.0 and the other two are < 1.0
    // For a sphere, the point lies on the unit sphere surface
    vec3 al = abs(hitLocal);
    float maxComp = max(al.x, max(al.y, al.z));
    float cubeScore = maxComp;  // cube: one axis = 1.0
    float sphereScore = abs(dot(hitLocal, hitLocal) - 1.0);  // sphere: r² = 1.0
    bool isSphere = (sphereScore < 0.15 && cubeScore < 0.98);
    if (isSphere) {
        // Sphere-like: normal = radial direction from center
        localNormal = normalize(hitLocal);
    } else {
        // Box: normal = closest axis face
        vec3 absLocal = abs(hitLocal);
        if (absLocal.x > absLocal.y && absLocal.x > absLocal.z) {
            localNormal = vec3(sign(hitLocal.x), 0, 0);
        } else if (absLocal.y > absLocal.z) {
            localNormal = vec3(0, sign(hitLocal.y), 0);
        } else {
            localNormal = vec3(0, 0, sign(hitLocal.z));
        }
    }

    // Transform normal to world space (use transpose of inverse for non-uniform scale)
    vec3 worldNormal = normalize(mat3(gl_ObjectToWorldEXT) * localNormal);

    // Always face the incoming ray (handle inside-out geometry like Cornell box walls)
    if (dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0) {
        worldNormal = -worldNormal;
    }
    payload.hitNormal = worldNormal;

    // Look up material from buffer: rgb=albedo, a=packed roughness (negative=metallic)
    vec4 matData = materialBuf.materials[gl_InstanceCustomIndexEXT];
    payload.hitAlbedo = matData.rgb;
    // Pack roughness + metallic into attenuation.x
    payload.attenuation = vec3(matData.a, 0.0, 0.0);  // attenuation.x = packed roughness
}
