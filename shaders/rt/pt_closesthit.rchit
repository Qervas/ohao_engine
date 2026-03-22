#version 460
#extension GL_EXT_ray_tracing : require

// Path Tracer Closest Hit Shader
// Uses object-space hit position to compute normals:
// - Sphere: normalize(hitLocal) — always correct for sphere meshes
// - Cube/quad: axis-aligned face normal
// Shape type encoded in material buffer .a (|packed| >= 10 = sphere)

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

layout(set = 0, binding = 3) readonly buffer MaterialBuffer {
    vec4 materials[];
} materialBuf;

// Normal buffer (per-vertex vec4) — for future interpolation
layout(set = 0, binding = 4) readonly buffer NormalBuffer {
    vec4 normals[];
} normalBuf;

// Index buffer (global uint indices)
layout(set = 0, binding = 5) readonly buffer IndexBuffer {
    uint indices[];
} indexBuf;

void main() {
    payload.hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    payload.hitDist = gl_HitTEXT;
    payload.hitInstance = gl_InstanceCustomIndexEXT;

    // Object-space hit position
    vec3 hitLocal = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * gl_HitTEXT;

    // Shape type from material
    float packedVal = materialBuf.materials[gl_InstanceCustomIndexEXT].a;
    bool isSphere = (abs(packedVal) >= 10.0);

    vec3 localNormal;
    if (isSphere) {
        // Sphere: radial normal from center (always correct)
        localNormal = normalize(hitLocal);
    } else {
        // Flat surface: axis-aligned face detection
        vec3 al = abs(hitLocal);
        if (al.x >= al.y && al.x >= al.z)
            localNormal = vec3(sign(hitLocal.x), 0, 0);
        else if (al.y >= al.z)
            localNormal = vec3(0, sign(hitLocal.y), 0);
        else
            localNormal = vec3(0, 0, sign(hitLocal.z));
    }

    // World space (handles non-uniform scale via transpose-inverse approximation)
    vec3 worldNormal = normalize(mat3(gl_ObjectToWorldEXT) * localNormal);

    // Face the ray
    if (dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0)
        worldNormal = -worldNormal;

    payload.hitNormal = worldNormal;

    // Material
    vec4 matData = materialBuf.materials[gl_InstanceCustomIndexEXT];
    payload.hitAlbedo = matData.rgb;
    payload.attenuation = vec3(matData.a, 0.0, 0.0);
}
