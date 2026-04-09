#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

// Path Tracer Closest Hit Shader — Bindless PBR
// Matches the GBuffer's bindless pattern: sampler2D textures[] + material indices

struct RayPayload {
    vec3 color;         // emissive output
    vec3 attenuation;   // x=roughness (negative=metallic), y=unused, z=unused
    vec3 hitPos;
    vec3 hitNormal;
    vec3 hitAlbedo;
    float hitDist;
    uint hitInstance;
};

layout(location = 0) rayPayloadInEXT RayPayload payload;
hitAttributeEXT vec2 baryCoord;

// Geometry buffers
layout(set = 0, binding = 3) readonly buffer MaterialBuffer { vec4 materials[]; } materialBuf;
layout(set = 0, binding = 4) readonly buffer NormalBuffer { vec4 normals[]; } normalBuf;
layout(set = 0, binding = 5) readonly buffer IndexBuffer { uint indices[]; } indexBuf;
layout(set = 0, binding = 8) readonly buffer UVBuffer { vec2 uvs[]; } uvBuf;
layout(set = 0, binding = 9) readonly buffer MatIDBuffer { uint matIDs[]; } matIDBuf;

// Material buffer: 2 vec4s per material
//   [matID*2+0] = (baseColor.rgb, diffuseTexIdx as float bits)
//   [matID*2+1] = (roughness, metallic, normalTexIdx as float bits, emissiveTexIdx as float bits)
layout(set = 0, binding = 10) readonly buffer MatColorBuffer { vec4 matColors[]; } matColorBuf;

// Bindless texture array — same pattern as GBuffer
layout(set = 0, binding = 11) uniform sampler2D textures[];

void main() {
    payload.hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    payload.hitDist = gl_HitTEXT;
    payload.hitInstance = gl_InstanceCustomIndexEXT;

    // Barycentric interpolation
    float u = baryCoord.x;
    float v = baryCoord.y;
    float w = 1.0 - u - v;

    // Global triangle/vertex lookup
    uint globalTriID = gl_InstanceCustomIndexEXT + gl_PrimitiveID;
    uint baseIdx = globalTriID * 3;
    uint i0 = indexBuf.indices[baseIdx + 0];
    uint i1 = indexBuf.indices[baseIdx + 1];
    uint i2 = indexBuf.indices[baseIdx + 2];

    // Interpolate UVs
    vec2 uv0 = uvBuf.uvs[i0];
    vec2 uv1 = uvBuf.uvs[i1];
    vec2 uv2 = uvBuf.uvs[i2];
    vec2 texUV = w * uv0 + u * uv1 + v * uv2;

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

    bool isThinGeometry = (dot(interpolated, interpolated) <= 0.0001);
    if (isThinGeometry && dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0)
        worldNormal = -worldNormal;

    // === Material lookup — 2 vec4s per material ===
    uint matID = matIDBuf.matIDs[globalTriID];
    vec4 matColor  = matColorBuf.matColors[matID * 2u + 0u];
    vec4 matParams = matColorBuf.matColors[matID * 2u + 1u];

    // Decode texture indices (stored as uint bits in float)
    uint diffuseTexIdx  = floatBitsToUint(matColor.a);
    uint normalTexIdx   = floatBitsToUint(matParams.z);
    uint emissiveTexIdx = floatBitsToUint(matParams.w);

    // === Albedo: sample diffuse texture or use base color ===
    vec3 albedo;
    if (diffuseTexIdx != 0xFFFFFFFFu) {
        albedo = texture(textures[nonuniformEXT(diffuseTexIdx)], texUV).rgb;
    } else {
        albedo = matColor.rgb;
    }

    // === Normal mapping via Frisvad basis (constructs TBN from normal alone) ===
    if (normalTexIdx != 0xFFFFFFFFu) {
        vec3 mapN = texture(textures[nonuniformEXT(normalTexIdx)], texUV).rgb;
        mapN = mapN * 2.0 - 1.0;  // [0,1] → [-1,1]

        // Frisvad orthonormal basis from single normal vector
        vec3 T, B;
        if (worldNormal.z < -0.9999) {
            T = vec3(0.0, -1.0, 0.0);
            B = vec3(-1.0, 0.0, 0.0);
        } else {
            float a = 1.0 / (1.0 + worldNormal.z);
            float d = -worldNormal.x * worldNormal.y * a;
            T = vec3(1.0 - worldNormal.x * worldNormal.x * a, d, -worldNormal.x);
            B = vec3(d, 1.0 - worldNormal.y * worldNormal.y * a, -worldNormal.y);
        }

        worldNormal = normalize(T * mapN.x + B * mapN.y + worldNormal * mapN.z);
    }

    payload.hitNormal = worldNormal;
    payload.hitAlbedo = albedo;

    // === PBR params ===
    float roughness = matParams.x;
    float metallic  = matParams.y;

    // === Emissive ===
    vec3 emissive = vec3(0.0);
    if (emissiveTexIdx != 0xFFFFFFFFu) {
        emissive = texture(textures[nonuniformEXT(emissiveTexIdx)], texUV).rgb;
    }
    payload.color = emissive;

    // Pack roughness + metallic for raygen
    payload.attenuation = vec3(metallic > 0.5 ? -(roughness + 0.001) : roughness, 0.0, 0.0);
}
