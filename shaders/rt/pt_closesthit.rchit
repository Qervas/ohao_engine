#version 460
#extension GL_EXT_ray_tracing : require

// Path Tracer Closest Hit Shader
// Interpolates per-vertex normals using barycentric coordinates.

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

layout(set = 0, binding = 4) readonly buffer NormalBuffer {
    vec4 normals[];
} normalBuf;

layout(set = 0, binding = 5) readonly buffer IndexBuffer {
    uint indices[];
} indexBuf;

void main() {
    payload.hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    payload.hitDist = gl_HitTEXT;
    payload.hitInstance = gl_InstanceCustomIndexEXT;

    // Barycentric interpolation of vertex normals
    float u = baryCoord.x;
    float v = baryCoord.y;
    float w = 1.0 - u - v;

    // gl_PrimitiveID is 0-based within each BLAS.
    // primitiveOffset in BLAS build maps it to the correct global index range.
    uint baseIdx = gl_PrimitiveID * 3;
    uint i0 = indexBuf.indices[baseIdx + 0];
    uint i1 = indexBuf.indices[baseIdx + 1];
    uint i2 = indexBuf.indices[baseIdx + 2];

    vec3 n0 = normalBuf.normals[i0].xyz;
    vec3 n1 = normalBuf.normals[i1].xyz;
    vec3 n2 = normalBuf.normals[i2].xyz;
    vec3 interpolated = w * n0 + u * n1 + v * n2;

    vec3 worldNormal;
    if (dot(interpolated, interpolated) > 0.0001) {
        // Use interpolated vertex normal — transform to world space
        worldNormal = normalize(mat3(gl_ObjectToWorldEXT) * normalize(interpolated));
    } else {
        // Fallback: geometric normal from object-space hit position
        vec3 hitLocal = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * gl_HitTEXT;
        vec3 al = abs(hitLocal);
        vec3 localN;
        if (al.x >= al.y && al.x >= al.z)
            localN = vec3(sign(hitLocal.x), 0, 0);
        else if (al.y >= al.z)
            localN = vec3(0, sign(hitLocal.y), 0);
        else
            localN = vec3(0, 0, sign(hitLocal.z));
        worldNormal = normalize(mat3(gl_ObjectToWorldEXT) * localN);
    }

    // Face the incoming ray
    if (dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0)
        worldNormal = -worldNormal;

    payload.hitNormal = worldNormal;

    // Material
    vec4 matData = materialBuf.materials[gl_InstanceCustomIndexEXT];
    payload.hitAlbedo = matData.rgb;
    payload.attenuation = vec3(matData.a, 0.0, 0.0);
}
