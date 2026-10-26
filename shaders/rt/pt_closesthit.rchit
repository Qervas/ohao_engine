#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "includes/scene/scene_data.glsl"
#include "shared/layout.glsl"

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

// Scene data from RenderScene (ohao/render/scene): mesh arenas, mesh table,
// per-triangle material slots, materials, instances and their material refs.
layout(set = 0, binding = 4, std430)  readonly buffer VertexArena { float vtx[]; };        // Vertex = 25 floats
layout(set = 0, binding = 5, std430)  readonly buffer IndexArena { uint idx[]; };          // mesh-local
layout(set = 0, binding = 8, std430)  readonly buffer MeshTable { MeshGpu meshes[]; };
layout(set = 0, binding = 9, std430)  readonly buffer TriangleSlots { uint triSlot[]; };
// Material table: 3 vec4s per material (layout::MaterialGpuPack)
//   [matID*3+0] = (baseColor.rgb, albedoTexIdx as float bits)
//   [matID*3+1] = (roughness, metallic, normalTexIdx bits, emissiveTexIdx bits)
//   [matID*3+2] = (roughMetalTexIdx bits, emissiveStrength, subsurface (raster SSS only),
//                  occlusionTexIdx bits (raster ambient only: a path tracer needs no baked AO))
layout(set = 0, binding = 10, std430) readonly buffer MatColorBuffer { vec4 matColors[]; } matColorBuf;
layout(set = 0, binding = 36, std430) readonly buffer Instances { InstanceGpu instances[]; };
layout(set = 0, binding = 37, std430) readonly buffer MaterialRefs { uint materialRefs[]; };

vec3 vtxNormal(uint v) { uint b = v * 25u + 6u; return vec3(vtx[b], vtx[b + 1u], vtx[b + 2u]); }
vec2 vtxUV(uint v)     { uint b = v * 25u + 9u; return vec2(vtx[b], vtx[b + 1u]); }

// Bindless texture array — the one table the G-buffer also reads (binding 12, variable
// count). Hit shaders have no derivatives: sample the base level (native resolution).
layout(set = 0, binding = 12) uniform sampler2D textures[];
// Binding 12's size (PathTracer::bindlessCapacity). An index at or past it is "no texture".
layout(constant_id = 1) const uint OHAO_BINDLESS_COUNT = 4096u;
uint boundTexture(uint idx) { return idx < OHAO_BINDLESS_COUNT ? idx : 0xFFFFFFFFu; }

void main() {
    payload.hitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    payload.hitDist = gl_HitTEXT;
    payload.hitInstance = gl_InstanceCustomIndexEXT;

    // Barycentric interpolation
    float u = baryCoord.x;
    float v = baryCoord.y;
    float w = 1.0 - u - v;

    // Instance slot -> mesh -> mesh-local indices
    InstanceGpu inst = instances[gl_InstanceCustomIndexEXT];
    MeshGpu mesh = meshes[inst.mesh];
    uint baseIdx = mesh.indexOffset + uint(gl_PrimitiveID) * 3u;
    uint i0 = mesh.vertexOffset + idx[baseIdx + 0u];
    uint i1 = mesh.vertexOffset + idx[baseIdx + 1u];
    uint i2 = mesh.vertexOffset + idx[baseIdx + 2u];

    // Interpolate UVs
    vec2 texUV = w * vtxUV(i0) + u * vtxUV(i1) + v * vtxUV(i2);

    // Interpolate vertex normals
    vec3 n0 = vtxNormal(i0);
    vec3 n1 = vtxNormal(i1);
    vec3 n2 = vtxNormal(i2);
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

    // A light's emitter mesh (kInstanceEmitter; only camera rays reach it): its radiance on the
    // front side, black behind, and the path ends there as at a miss. The light itself is the
    // analytic one the raygen samples; this is what the camera sees of it.
    if ((inst.flags & kInstanceEmitter) != 0u) {
        payload.color = dot(worldNormal, gl_WorldRayDirectionEXT) < 0.0 ? inst.giAlbedoPacked.rgb : vec3(0.0);
        payload.hitDist = -1.0;
        payload.hitNormal = worldNormal;
        payload.hitAlbedo = vec3(0.0);
        payload.attenuation = vec3(1.0, 0.0, 0.0);
        return;
    }

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
    uint slot = triSlot[mesh.triangleSlotOffset + uint(gl_PrimitiveID)];
    uint matID = OHAO_MATERIAL_INDEX(inst, slot, materialRefs);
    vec4 matColor   = matColorBuf.matColors[matID * 3u + 0u];  // (baseColor.rgb, diffuseTexIdx)
    vec4 matParams  = matColorBuf.matColors[matID * 3u + 1u];  // (roughness, metallic, normalTexIdx, emissiveTexIdx)
    vec4 matParams2 = matColorBuf.matColors[matID * 3u + 2u];  // (roughMetalTexIdx, emissiveStrength, subsurface, occlusionTexIdx)

    // Decode texture indices (stored as uint bits in float)
    uint diffuseTexIdx    = boundTexture(floatBitsToUint(matColor.a));
    uint normalTexIdx     = boundTexture(floatBitsToUint(matParams.z));
    uint emissiveTexIdx   = boundTexture(floatBitsToUint(matParams.w));
    uint roughMetalTexIdx = boundTexture(floatBitsToUint(matParams2.x));

    // === Albedo: the base color, times the albedo texture when there is one ===
    // Albedo textures are R8G8B8A8_SRGB in the shared bindless table: the sampler
    // decodes to linear. An untextured material is its base color, once.
    vec3 albedo = matColor.rgb;
    if (diffuseTexIdx != 0xFFFFFFFFu) {
        albedo *= textureLod(textures[nonuniformEXT(diffuseTexIdx)], texUV, 0.0).rgb;
    }

    // === Normal mapping via Frisvad basis (constructs TBN from normal alone) ===
    if (normalTexIdx != 0xFFFFFFFFu) {
        vec3 mapN = textureLod(textures[nonuniformEXT(normalTexIdx)], texUV, 0.0).rgb;
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
    if (roughMetalTexIdx != 0xFFFFFFFFu) {
        vec4 rm = textureLod(textures[nonuniformEXT(roughMetalTexIdx)], texUV, 0.0);
        roughness *= rm.g;  // G channel = Roughness
        metallic  *= rm.b;  // B channel = Metallic
    }
    roughness = max(roughness, 0.04);

    // === Emissive ===
    vec3 emissive = vec3(0.0);
    if (emissiveTexIdx != 0xFFFFFFFFu) {
        // R8G8B8A8_SRGB: the sampler decodes to linear. Times the material's emissive
        // strength, as the deferred frame's G-buffer emits (core/gbuffer.frag).
        emissive = textureLod(textures[nonuniformEXT(emissiveTexIdx)], texUV, 0.0).rgb * matParams2.y;
    }
    payload.color = emissive;

    // Continuous metal-rough for raygen (scalar full PBR / inverse).
    // .x = roughness, .y = metallic, .z = per-pixel curvature [0,1].
    payload.attenuation = vec3(roughness, clamp(metallic, 0.0, 1.0), curv);
}
