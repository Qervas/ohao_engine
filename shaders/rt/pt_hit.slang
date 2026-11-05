// The path tracer's hits (sub-project 25, W1; docs/specs/2026-11-05-wavefront-path-tracer-design.md): what a ray meets,
// decoded from the scene's buffers into a PathPayload, and whether a candidate hit counts (alpha, panes), as functions
// of a HitContext that a hit shader's intrinsics or a ray query's committed or candidate hit fills. The megakernel's
// hit shaders (rt/path_tracer.slang) and the wavefront's ray-query kernels decode hits with the same code.
import shared.layout;
import material.openpbr;
import material.material_layers;
import rt.lights;

[[vk::binding(4, 0)]] StructuredBuffer<float> vertices;       // Vertex: 25 floats
[[vk::binding(5, 0)]] StructuredBuffer<uint> indices;         // mesh-local
[[vk::binding(8, 0)]] StructuredBuffer<uint4> meshes;         // vertexOffset, indexOffset, indexCount, triangleSlotOffset
[[vk::binding(9, 0)]] StructuredBuffer<uint> triangleSlots;
[[vk::binding(36, 0)]] StructuredBuffer<float4> instances;    // InstanceGpu, kInstanceGpuStride / 16 rows
[[vk::binding(37, 0)]] StructuredBuffer<uint> materialRefs;
[[vk::binding(38, 0)]] StructuredBuffer<float> openpbrTables; // OpenPBRTables (kPbrTableFloats)

static const uint kVertexFloats = 25u;
static const uint kNoTex = 0xFFFFFFFFu;

// ---- the payload ---------------------------------------------------------------------------

static const uint kHit = 1u;
static const uint kHitEmitter = 2u;
static const uint kHitEmissive = 4u;   // an emissive triangle light (payload.light its index)
static const uint kHitBack = 8u;       // the ray leaves a closed mesh (met from inside: along its outward normal)
// The path tracer's shadow and bounce rays: the scene and the blended instances (kTlasMaskTransparent:
// glass, panes), which the hybrid frame's rays leave out but a path must meet (anyHit decides a
// blended surface stochastically; glass is shaded). Without them a ray inside a glass ball never met
// its far side, refracting once: the lens was too weak, and glass cast no shadow.
static const uint kPathMask = kRayMaskScene | kTlasMaskTransparent;
static const uint kCrossings = 16u;    // glass events (refractions, reflections inside) beyond the bounces
static const uint kShadowRay = 16u;    // visible()'s ray: anyHit lets a pane through (paneMaterial), times its transmission
static const uint kHitCulled = 64u;    // the back face of a one-sided material (the raster culls it)
static const uint kCullSkips = 8u;     // culled faces a camera ray passes

// What a ray brings back: a surface's place and its OpenPBR inputs (resolved from its material
// and textures by the closest hit), a light's emitter (its light), or nothing (a miss).
struct PathPayload
{
    float3 position;
    float t;
    float3 geoNormal;      // facing the ray
    uint flags;
    float3 normal;         // shading, facing the ray's side
    uint light;            // an emitter's light index, or an emissive triangle's (kHitEmissive)
    float3 baseColor;
    float baseWeight;
    float3 specularColor;
    float specularWeight;
    float3 emission;
    float metalness;
    float diffuseRoughness;
    float specularRoughness;
    float specularIor;
    uint material;         // the hit's material: its layer rows (material/material_layers.slang) at uv
    float2 uv;
    float3 tangent;        // an anisotropic material's tangent (rotated, in the plane of normal); else 0
    float pad;
};

// The hit's context: what a closest-hit shader reads from its intrinsics, or a ray query from its committed hit.
struct HitContext
{
    uint instance;            // the instance's custom index (its slot)
    uint primitive;
    float2 bary;
    float3 origin, dir;       // the ray in world space
    float t;
    float3x3 objectToWorld, worldToObject;
    uint2 launch;             // the pixel (a blended surface's coin)
}

uint pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// A pane (ruling O14): a thin-walled transmissive surface smooth enough to pass light straight through
// (specular_roughness at most kPaneRough, by its constants, as anyHit and the path both read them).
// Shadow rays pass panes, tinted by what each transmits; a bounce ray that refracts through one keeps
// the last vertex's density for MIS (a null crossing), so light through a window is light sampled and
// BSDF sampled once each. Rougher thin walls and solids still block shadow rays: their light arrives by
// bounce rays alone, in full.
static const float kPaneRough = 0.05;

bool paneMaterial(uint material)
{
    const uint m = material * kMaterialRows;
    const float4 r1 = materials[m + 1u], r12 = materials[m + kTransmissionRowFirst];
    const float4 r13 = materials[m + kTransmissionRowFirst + 1u];
    return r12.w > 0.0 && r13.y > 0.5 && r1.y < 1.0 && r1.x <= kPaneRough;
}

struct PathTextures : IMaterialTextures
{
    float4 sample(uint index, float2 uv) { return sampleTex(index, uv); }
};

MaterialInputs inputsOf(PathPayload p)
{
    MaterialInputs m = defaultMaterialInputs();
    m.baseColor = p.baseColor;
    m.baseWeight = p.baseWeight;
    m.baseMetalness = p.metalness;
    m.baseDiffuseRoughness = p.diffuseRoughness;
    m.specularWeight = p.specularWeight;
    m.specularColor = p.specularColor;
    m.specularRoughness = p.specularRoughness;
    m.specularIor = p.specularIor;
    const uint r = p.material * kMaterialRows + kLayerRowFirst;
    PathTextures tex;
    applyLayerRows(m, materials[r], materials[r + 1u], materials[r + 2u], materials[r + 3u], materials[r + 4u], p.uv, tex);
    const uint t = p.material * kMaterialRows + kTransmissionRowFirst;
    applyTransmissionRows(m, materials[t], materials[t + 1u], materials[t + 2u]);
    applySubsurfaceRows(m, materials[p.material * kMaterialRows + 2u], materials[p.material * kMaterialRows + kSubsurfaceRowFirst]);
    m.subsurfaceLocal = false;   // walked: lobe 5 (raygen's walk)
    return m;
}

float vertexFloat(uint v, uint k) { return vertices[v * kVertexFloats + k]; }
float3 vertexF3(uint v, uint k) { return float3(vertexFloat(v, k), vertexFloat(v, k + 1u), vertexFloat(v, k + 2u)); }

struct Corners
{
    uint v0, v1, v2;
    uint material;
    uint flags;
    float4 instanceRow9;
};

Corners cornersOf(uint slot, uint primitive)
{
    const uint stride = kInstanceGpuStride / 16u;
    const uint4 info = asuint(instances[slot * stride + 8u]);   // mesh, refOffset, refCount, flags
    const uint4 mesh = meshes[info.x];
    const uint baseIndex = mesh.y + primitive * 3u;
    Corners c;
    c.v0 = mesh.x + indices[baseIndex];
    c.v1 = mesh.x + indices[baseIndex + 1u];
    c.v2 = mesh.x + indices[baseIndex + 2u];
    const uint triSlot = triangleSlots[mesh.w + primitive];
    c.material = triSlot < info.z ? materialRefs[info.y + triSlot] : 0u;
    c.flags = info.w;
    c.instanceRow9 = instances[slot * stride + 9u];
    return c;
}

float4 sampleTex(uint index, float2 uv) { return textures[NonUniformResourceIndex(index)].SampleLevel(uv, 0.0); }

// The hit a ray meets, decoded into the payload: its place, normals and OpenPBR inputs (from the context a closest-hit
// shader or a ray query's committed hit gives).
void fillHitAt(inout PathPayload payload, HitContext hc)
{
    const float2 bary = hc.bary;
    const float w = 1.0 - bary.x - bary.y;
    const Corners c = cornersOf(hc.instance, hc.primitive);
    const float3 dir = hc.dir;
    payload.t = hc.t;
    payload.position = hc.origin + dir * payload.t;
    payload.flags = kHit;

    // The geometric normal from the triangle's corners, and the interpolated shading normal,
    // into world space (normals by the inverse transpose), both turned toward the ray.
    const float3x3 w2o = hc.worldToObject;
    const float3 p0 = vertexF3(c.v0, 0u), p1 = vertexF3(c.v1, 0u), p2 = vertexF3(c.v2, 0u);
    float3 geo = normalize(mul(cross(p1 - p0, p2 - p0), w2o));
    const float3 nObj = vertexF3(c.v0, 6u) * w + vertexF3(c.v1, 6u) * bary.x + vertexF3(c.v2, 6u) * bary.y;
    float3 n = dot(nObj, nObj) > 1e-8 ? normalize(mul(nObj, w2o)) : geo;
    const bool front = dot(geo, dir) < 0.0;   // the side the winding faces (emitters emit from it)
    // What the raster culls (glTF's doubleSided false, material row 10's w): the side the world-space
    // winding faces away from the ray; a mirroring transform turns the winding (the normal's inverse
    // transpose does not).
    {
        const bool mirrored = determinant(hc.objectToWorld) < 0.0;
        if (front == mirrored && materials[c.material * kMaterialRows + 10u].w > 0.5) payload.flags |= kHitCulled;
    }
    if (!front) geo = -geo;
    if (dot(n, geo) < 0.0) n = -n;
    payload.geoNormal = geo;
    // From inside: the ray runs along the outward normal, the interpolated vertex normal's side (a
    // winding is not the same across assets: the generated sphere's is inverted, which shaded every
    // glass ball's entry as an exit, refracting the wrong way and absorbing outside it); a mesh
    // without vertex normals by its winding.
    if (dot(nObj, nObj) > 1e-8 ? dot(mul(nObj, w2o), dir) > 0.0 : !front) payload.flags |= kHitBack;

    if ((c.flags & kInstanceEmitter) != 0u)
    {
        payload.flags |= kHitEmitter;
        payload.light = uint(c.instanceRow9.w + 0.5);
        payload.normal = geo;
        payload.emission = front ? c.instanceRow9.rgb : float3(0.0);
        return;
    }

    const uint m = c.material * kMaterialRows;
    const float4 row0 = materials[m], row1 = materials[m + 1u], row2 = materials[m + 2u];
    const float4 row4 = materials[m + 4u], row5 = materials[m + 5u], row6 = materials[m + 6u];
    const float2 uv = float2(vertexFloat(c.v0, 9u), vertexFloat(c.v0, 10u)) * w +
                      float2(vertexFloat(c.v1, 9u), vertexFloat(c.v1, 10u)) * bary.x +
                      float2(vertexFloat(c.v2, 9u), vertexFloat(c.v2, 10u)) * bary.y;

    // A normal map in the vertex tangents' frame, as the G-buffer reads it.
    const uint normalTex = asuint(row1.z);
    if (normalTex < 4096u)
    {
        const float4 tObj = float4(vertexF3(c.v0, 13u), vertexFloat(c.v0, 16u)) * w +
                            float4(vertexF3(c.v1, 13u), vertexFloat(c.v1, 16u)) * bary.x +
                            float4(vertexF3(c.v2, 13u), vertexFloat(c.v2, 16u)) * bary.y;
        float3 tw = mul(hc.objectToWorld, tObj.xyz);
        float3 T, B;
        if (dot(tw, tw) > 1e-12)
        {
            T = normalize(tw - n * dot(n, tw));
            B = normalize(cross(n, T)) * (vertexFloat(c.v0, 16u) < 0.0 ? -1.0 : 1.0);
        }
        else
        {
            basisOf(n, T, B);
        }
        const float3 tn = sampleTex(normalTex, uv).rgb * 2.0 - 1.0;
        const float3 mapped = T * tn.x + B * tn.y + n * tn.z;
        if (dot(mapped, mapped) > 1e-8) n = normalize(mapped);
        if (dot(n, geo) < 1e-3) n = normalize(n + geo * (1e-3 - dot(n, geo)));
    }
    payload.normal = n;

    float3 baseColor = row0.rgb;
    const uint albedoTex = asuint(row0.w);
    if (albedoTex < 4096u) baseColor *= sampleTex(albedoTex, uv).rgb;
    float roughness = row1.x, metalness = row1.y;
    const uint roughMetalTex = asuint(row2.x);
    if (roughMetalTex < 4096u)
    {
        const float4 rm = sampleTex(roughMetalTex, uv);
        roughness *= rm.g;
        metalness *= rm.b;
    }
    float specularWeight = row4.w;
    float3 specularColor = row4.rgb;
    const uint specTex = asuint(row5.w);
    if (specTex < 4096u) specularWeight *= sampleTex(specTex, uv).a;
    const uint specColorTex = asuint(row6.w);
    if (specColorTex < 4096u) specularColor *= sampleTex(specColorTex, uv).rgb;
    // Emission: emission_color x emission_luminance, times the emissive texture when there is one.
    float3 emission = row6.rgb * row2.y;
    const uint emissiveTex = asuint(row1.w);
    if (emissiveTex < 4096u) emission *= sampleTex(emissiveTex, uv).rgb;

    // An emissive triangle is a light (render/rt/emissive_lights.hpp): its index, for the bounce's weight.
    if (any(emission > 0.0))
    {
        const uint first = emissiveFirst[hc.instance];
        if (first != ~0u)
        {
            payload.flags |= kHitEmissive;
            payload.light = first + hc.primitive;
        }
    }
    payload.baseColor = baseColor;
    payload.baseWeight = row5.z;
    payload.metalness = saturate(metalness);
    payload.diffuseRoughness = row5.y;
    payload.specularWeight = specularWeight;
    payload.specularColor = specularColor;
    payload.specularRoughness = saturate(roughness);
    payload.specularIor = row5.x;
    payload.emission = emission;
    payload.material = c.material;
    payload.uv = uv;
    // Anisotropy's tangent: the vertex tangents' (as a normal map reads them), else any in the plane
    // of n, turned by geometry_tangent_rotation.
    payload.tangent = float3(0.0);
    const float4 row10 = materials[m + 10u];
    if (row10.y > 0.0 || (materials[m + 7u].w > 0.0 && materials[m + 8u].y > 0.0))
    {
        const float4 tObj = float4(vertexF3(c.v0, 13u), vertexFloat(c.v0, 16u)) * w +
                            float4(vertexF3(c.v1, 13u), vertexFloat(c.v1, 16u)) * bary.x +
                            float4(vertexF3(c.v2, 13u), vertexFloat(c.v2, 16u)) * bary.y;
        const float3 tw = mul(hc.objectToWorld, tObj.xyz);
        const float3 sn = payload.normal;
        float3 T, B;
        if (dot(tw, tw) > 1e-12 && dot(tw - sn * dot(sn, tw), tw - sn * dot(sn, tw)) > 1e-12) T = normalize(tw - sn * dot(sn, tw));
        else basisOf(sn, T, B);
        payload.tangent = rotatedTangent(sn, T, row10.z);
    }
}


// Whether a ray keeps a candidate hit (the any-hit shader's rule, for a ray query's candidate loop too): an opaque
// material always; a masked one by its cutoff; a blended one stochastically (a hash of the launch, the triangle and
// the ray); a pane a shadow ray passes is let through, multiplying what it transmits at the ray's angle into
// `transmission` (kept as a hit once nothing gets through).
bool anyHitAccepts(HitContext hc, bool shadowRay, inout float3 transmission)
{
    const Corners c = cornersOf(hc.instance, hc.primitive);
    if (shadowRay && paneMaterial(c.material))
    {
        // Its straight transmission at the ray's angle (the geometry's NO_DUPLICATE_ANY_HIT flag: once each).
        PathPayload h;
        h.flags = 0u;
        fillHitAt(h, hc);
        OpenPBRTables tables;
        tables.data = openpbrTables;
        const float cs = clamp(abs(dot(hc.dir, h.geoNormal)), 1e-4, 1.0);
        transmission *= OpenPBRSurface(inputsOf(h)).transmissionAlbedo(float3(sqrt(1.0 - cs * cs), 0.0, cs), tables);
        return !any(transmission > 0.0);   // let through (ignored) while anything gets through
    }
    const uint m = c.material * kMaterialRows;
    const float4 row3 = materials[m + 3u];   // mode, cutoff, opacity, base alpha
    const uint mode = uint(row3.x + 0.5);
    if (mode == kAlphaOpaque) return true;
    float alpha = row3.w * row3.z;
    const uint albedoTex = asuint(materials[m].w);
    if (albedoTex < 4096u)
    {
        const float2 bary = hc.bary;
        const float w = 1.0 - bary.x - bary.y;
        const float2 uv = float2(vertexFloat(c.v0, 9u), vertexFloat(c.v0, 10u)) * w +
                          float2(vertexFloat(c.v1, 9u), vertexFloat(c.v1, 10u)) * bary.x +
                          float2(vertexFloat(c.v2, 9u), vertexFloat(c.v2, 10u)) * bary.y;
        alpha *= sampleTex(albedoTex, uv).a;
    }
    if (mode == kAlphaMask)
    {
        return !(alpha < row3.y);
    }
    // Blended: stochastically present, by a hash of the ray and the triangle.
    const uint2 launch = hc.launch;
    uint h = pcg(launch.x ^ pcg(launch.y ^ pcg(hc.primitive ^ pcg(hc.instance))));
    h = pcg(h ^ asuint(hc.origin.x) ^ pcg(asuint(hc.origin.y) ^ asuint(hc.dir.z)));
    return float(h >> 8u) * (1.0 / 16777216.0) < alpha;
}
