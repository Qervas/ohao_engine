#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

// Path Tracer Closest Hit Shader
// Per-vertex normal + UV interpolation, per-triangle material, texture sampling

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

layout(set = 0, binding = 3) readonly buffer MaterialBuffer { vec4 materials[]; } materialBuf;
layout(set = 0, binding = 4) readonly buffer NormalBuffer { vec4 normals[]; } normalBuf;
layout(set = 0, binding = 5) readonly buffer IndexBuffer { uint indices[]; } indexBuf;
layout(set = 0, binding = 8) readonly buffer UVBuffer { vec2 uvs[]; } uvBuf;
layout(set = 0, binding = 9) readonly buffer MatIDBuffer { uint matIDs[]; } matIDBuf;
layout(set = 0, binding = 10) readonly buffer MatColorBuffer { vec4 matColors[]; } matColorBuf;
layout(set = 0, binding = 11) uniform sampler2DArray textureArray;

void main() {
    payload.hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    payload.hitDist = gl_HitTEXT;
    payload.hitInstance = gl_InstanceCustomIndexEXT;

    // Barycentric interpolation
    float u = baryCoord.x;
    float v = baryCoord.y;
    float w = 1.0 - u - v;

    uint baseIdx = gl_PrimitiveID * 3;
    uint i0 = indexBuf.indices[baseIdx + 0];
    uint i1 = indexBuf.indices[baseIdx + 1];
    uint i2 = indexBuf.indices[baseIdx + 2];

    // Interpolate vertex normals
    vec3 n0 = normalBuf.normals[i0].xyz;
    vec3 n1 = normalBuf.normals[i1].xyz;
    vec3 n2 = normalBuf.normals[i2].xyz;
    vec3 interpolated = w * n0 + u * n1 + v * n2;

    vec3 worldNormal;
    if (dot(interpolated, interpolated) > 0.0001) {
        worldNormal = normalize(mat3(gl_ObjectToWorldEXT) * normalize(interpolated));
    } else {
        vec3 hitLocal = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * gl_HitTEXT;
        vec3 al = abs(hitLocal);
        vec3 localN;
        if (al.x >= al.y && al.x >= al.z) localN = vec3(sign(hitLocal.x), 0, 0);
        else if (al.y >= al.z) localN = vec3(0, sign(hitLocal.y), 0);
        else localN = vec3(0, 0, sign(hitLocal.z));
        worldNormal = normalize(mat3(gl_ObjectToWorldEXT) * localN);
    }

    // Only flip normals for thin geometry
    bool isThinGeometry = (dot(interpolated, interpolated) <= 0.0001);
    if (isThinGeometry && dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0)
        worldNormal = -worldNormal;

    payload.hitNormal = worldNormal;

    // Per-triangle material lookup
    // gl_InstanceCustomIndexEXT = global triangle offset for this instance
    uint matID = matIDBuf.matIDs[gl_InstanceCustomIndexEXT + gl_PrimitiveID];
    vec4 matColor = matColorBuf.matColors[matID];

    // Check if this material has a texture (layer index stored in .a, -1 = no texture)
    float texLayer = matColor.a;
    vec3 albedo;

    if (texLayer >= 0.0) {
        // Interpolate UVs
        vec2 uv0 = uvBuf.uvs[i0];
        vec2 uv1 = uvBuf.uvs[i1];
        vec2 uv2 = uvBuf.uvs[i2];
        vec2 texUV = w * uv0 + u * uv1 + v * uv2;

        // Sample texture array at the material's layer
        albedo = texture(textureArray, vec3(texUV, texLayer)).rgb;
        // Convert from sRGB to linear (the texture is stored as SRGB format, Vulkan handles this)
    } else {
        // No texture — use material base color
        albedo = matColor.rgb;
    }

    payload.hitAlbedo = albedo;

    // Roughness: default matte for organic materials
    // Per-instance material buffer no longer indexed by customIndex (it's now triOffset)
    // Use a fixed roughness for now — TODO: pass roughness per material
    payload.attenuation = vec3(0.75, 0.0, 0.0);  // matte default
}
