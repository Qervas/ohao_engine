#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require

// Path Tracer Closest Hit Shader — Bindless PBR
// Matches the GBuffer's bindless pattern: sampler2D textures[] + material indices

struct RayPayload {
    vec3 color;         // emissive output
    vec3 attenuation;   // x=roughness, y=metallic (continuous), z=curvature
    vec3 hitPos;
    vec3 hitNormal;
    vec3 hitAlbedo;
    float hitDist;
    uint hitInstance;
    float envPdf;       // set by miss shader when env map sampled; used for MIS weighting
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

// Bindless texture array — same pattern as GBuffer (binding 12 = last, variable count)
layout(set = 0, binding = 12) uniform sampler2D textures[];

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

    mat3 normalMatrix = transpose(inverse(mat3(gl_ObjectToWorldEXT)));
    vec3 worldNormal;
    if (dot(interpolated, interpolated) > 0.0001) {
        worldNormal = normalize(normalMatrix * normalize(interpolated));
    } else {
        vec3 hitLocal = gl_ObjectRayOriginEXT + gl_ObjectRayDirectionEXT * gl_HitTEXT;
        vec3 al = abs(hitLocal);
        vec3 localN;
        if (al.x >= al.y && al.x >= al.z) localN = vec3(sign(hitLocal.x), 0, 0);
        else if (al.y >= al.z) localN = vec3(0, sign(hitLocal.y), 0);
        else localN = vec3(0, 0, sign(hitLocal.z));
        worldNormal = normalize(normalMatrix * localN);
    }

    bool isThinGeometry = (dot(interpolated, interpolated) <= 0.0001);
    if (isThinGeometry && dot(worldNormal, gl_WorldRayDirectionEXT) > 0.0)
        worldNormal = -worldNormal;

    // 4.M: per-pixel curvature proxy from vertex-normal variance. High when
    // the 3 vertex normals diverge (corners, edges, ears, nostrils); near zero
    // on flat or smoothly-curved regions. Used by SSS in raygen to modulate
    // Gaussian wrap width — high curvature = thinner geometry = more bleed.
    // We can't use screen-space derivatives in RT, this is the cheap analog.
    vec3 nn0 = normalize(n0);
    vec3 nn1 = normalize(n1);
    vec3 nn2 = normalize(n2);
    float curv = (1.0 - dot(nn0, nn1)) + (1.0 - dot(nn1, nn2)) + (1.0 - dot(nn0, nn2));
    curv = clamp(curv * 8.0, 0.0, 1.0);  // scale so typical face values land 0..1

    // === Material lookup — 3 vec4s per material ===
    uint matID = matIDBuf.matIDs[globalTriID];
    vec4 matColor   = matColorBuf.matColors[matID * 3u + 0u];  // (baseColor.rgb, diffuseTexIdx)
    vec4 matParams  = matColorBuf.matColors[matID * 3u + 1u];  // (roughness, metallic, normalTexIdx, emissiveTexIdx)
    vec4 matParams2 = matColorBuf.matColors[matID * 3u + 2u];  // (roughMetalTexIdx, unused, unused, unused)

    // Decode texture indices (stored as uint bits in float)
    uint diffuseTexIdx    = floatBitsToUint(matColor.a);
    uint normalTexIdx     = floatBitsToUint(matParams.z);
    uint emissiveTexIdx   = floatBitsToUint(matParams.w);
    uint roughMetalTexIdx = floatBitsToUint(matParams2.x);

    // === Albedo: sample diffuse texture or use base color ===
    vec3 albedo = matColor.rgb;
    if (diffuseTexIdx != 0xFFFFFFFFu) {
        vec3 sampled = texture(textures[nonuniformEXT(diffuseTexIdx)], texUV).rgb;
        // RT texture array is UNORM, manual sRGB to Linear conversion
        albedo *= pow(sampled, vec3(2.2));
    }

    // === Normal mapping via Frisvad basis (constructs TBN from normal alone) ===
    if (normalTexIdx != 0xFFFFFFFFu) {
        vec3 mapN = texture(textures[nonuniformEXT(normalTexIdx)], texUV).rgb;
        mapN = normalize(mapN * 2.0 - 1.0);  // [0,1] → [-1,1] and normalize for stability

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

    // === PBR params: per-pixel from texture or scalar fallback ===
    float roughness = matParams.x;
    float metallic  = matParams.y;
    float ao        = 1.0;
    if (roughMetalTexIdx != 0xFFFFFFFFu) {
        vec4 rm = texture(textures[nonuniformEXT(roughMetalTexIdx)], texUV);
        ao        *= rm.r;  // R channel = AO
        roughness *= rm.g;  // G channel = Roughness
        metallic  *= rm.b;  // B channel = Metallic
    }
    roughness = max(roughness, 0.04);

    // === Emissive ===
    vec3 emissive = vec3(0.0);
    if (emissiveTexIdx != 0xFFFFFFFFu) {
        vec3 sampled = texture(textures[nonuniformEXT(emissiveTexIdx)], texUV).rgb;
        // RT texture array is UNORM, manual sRGB to Linear conversion
        emissive = pow(sampled, vec3(2.2));
    }
    payload.color = emissive;

    // Continuous metal-rough for raygen (scalar full PBR / inverse).
    // .x = roughness, .y = metallic, .z = per-pixel curvature [0,1].
    payload.attenuation = vec3(roughness, clamp(metallic, 0.0, 1.0), curv);
}
