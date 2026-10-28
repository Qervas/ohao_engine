// The decals pass (sub-project 12, step 3): each live decal record is a unit cube of decal space
// drawn as its back faces (cull front; six faces, counter-clockwise from outside), into the G-buffer's
// albedo and normal targets after gbuffer.late. A pixel is decaled only if the surface the G-buffer
// holds there lies inside the box: the position target gives it, so no depth test is needed.
// Rejected: the sky (depth 1), an emitter mesh (metallic -1), anything nearer the eye than the
// clearance (the first-person gun's reach), and a surface more than 60 degrees off the decal's
// normal (the threshold inclusive). That normal is the position target's finite-difference normal:
// this pass writes the G-buffer's normal, so it cannot read it.
// Albedo blends by the texture's alpha; the normal and roughness are replaced where that alpha is
// at least 0.5, because the G-buffer's normal is octahedral and does not blend (OPEN ruling 16).
// The normal map (linear) is rotated into the decal's frame: its T and B are the decal axes.
import shared.layout;

[[vk::binding(0, 0)]] StructuredBuffer<DecalGpu> decals;
[[vk::binding(1, 0)]] Texture2D<float4> gPosition;   // xyz world position, w metallic (-1: an emitter)
[[vk::binding(2, 0)]] Texture2D<float> sceneDepth;   // the G-buffer's depth (1 on the sky)
[[vk::binding(0, 1)]] Sampler2D textures[4096];      // the bindless table (sized: see transparent_mesh.slang)
[[vk::push_constant]] ConstantBuffer<DecalPush> pc;

static const float2 kCorners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };

float3 axisVec(uint i)
{
    return float3(i == 0u ? 1.0 : 0.0, i == 1u ? 1.0 : 0.0, i == 2u ? 1.0 : 0.0);
}

struct VsOut
{
    float4 position : SV_Position;
    [[vk::location(0)]] nointerpolation uint decal;
};

[shader("vertex")]
VsOut vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    // Face f has normal s * e_axis; (u, v) with u x v = that normal, so the two triangles are
    // counter-clockwise seen from outside (the cull keeps the far faces).
    const uint face = vertexId / 6u;
    const uint axis = face / 2u;
    const float s = (face % 2u) == 0u ? 1.0 : -1.0;
    const uint b = (axis + 1u) % 3u;
    const uint c = (axis + 2u) % 3u;
    const float3 n = axisVec(axis) * s;
    const float3 u = s > 0.0 ? axisVec(b) : axisVec(c);
    const float3 v = s > 0.0 ? axisVec(c) : axisVec(b);
    const float2 corner = kCorners[vertexId % 6u];
    const float3 local = n * 0.5 + u * (corner.x * 0.5) + v * (corner.y * 0.5);
    const DecalGpu d = decals[instanceId];
    VsOut o;
    o.position = mul(pc.viewProj, mul(d.decalToWorld, float4(local, 1.0)));
    o.decal = instanceId;
    return o;
}

struct PsOut
{
    [[vk::location(0)]] float4 albedo;      // rgb tinted albedo, a the blend factor (blended)
    [[vk::location(1)]] float4 normalRough; // the replaced normal (octahedral, *0.5+0.5) and roughness
};

float2 signNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// core/gbuffer's encodeNormalOctahedron (includes/scene/gbuffer_body.glsl), exactly.
float2 encodeOctahedron(float3 n)
{
    float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    if (n.z < 0.0)
        p = (1.0 - abs(p.yx)) * signNotZero(p);
    return p;
}

// A G-buffer pixel holding a surface a decal may land on.
bool surfaceAt(int2 q, int2 maxP)
{
    const int2 c = clamp(q, int2(0, 0), maxP);
    return sceneDepth.Load(int3(c, 0)) < 1.0 && gPosition.Load(int3(c, 0)).w >= -0.5;
}

[shader("fragment")]
PsOut fragmentMain(VsOut i)
{
    const int2 p = int2(i.position.xy);
    const int2 maxP = int2(pc.screen.xy) - int2(1, 1);
    if (!surfaceAt(p, maxP)) discard;   // the sky, or an emitter's mesh
    const float3 P = gPosition.Load(int3(p, 0)).xyz;
    const float3 eye = pc.camera.xyz;
    if (distance(P, eye) < pc.screen.z) discard;   // the first-person gun's reach

    const DecalGpu d = decals[i.decal];
    const float3 q = mul(d.worldToDecal, float4(P, 1.0)).xyz;
    if (abs(q.x) > 0.5 || abs(q.y) > 0.5 || abs(q.z) > 0.5) discard;   // outside the box

    // The surface's normal from its neighbours' positions, toward the eye; a one-sided difference
    // at an edge of the surface, none where neighbours are all gone (the decal's normal then passes).
    const float3 nd = normalize(mul(d.decalToWorld, float4(0.0, 0.0, -1.0, 0.0)).xyz);
    const bool hasR = surfaceAt(p + int2(1, 0), maxP), hasL = surfaceAt(p - int2(1, 0), maxP);
    const bool hasU = surfaceAt(p + int2(0, 1), maxP), hasD = surfaceAt(p - int2(0, 1), maxP);
    float3 dx = float3(0.0), dy = float3(0.0);
    if (hasR && hasL) dx = gPosition.Load(int3(p + int2(1, 0), 0)).xyz - gPosition.Load(int3(p - int2(1, 0), 0)).xyz;
    else if (hasR) dx = gPosition.Load(int3(p + int2(1, 0), 0)).xyz - P;
    else if (hasL) dx = P - gPosition.Load(int3(p - int2(1, 0), 0)).xyz;
    if (hasU && hasD) dy = gPosition.Load(int3(p + int2(0, 1), 0)).xyz - gPosition.Load(int3(p - int2(0, 1), 0)).xyz;
    else if (hasU) dy = gPosition.Load(int3(p + int2(0, 1), 0)).xyz - P;
    else if (hasD) dy = P - gPosition.Load(int3(p - int2(0, 1), 0)).xyz;
    float3 N = cross(dx, dy);
    if (length(N) > 1e-12)
    {
        N = normalize(N);
        if (dot(N, eye - P) < 0.0) N = -N;
        if (dot(N, nd) < 0.5) discard;   // more than 60 degrees off the decal's normal
    }
    else
    {
        N = nd;
    }

    const float2 uv = float2(q.x + 0.5, 0.5 - q.y);
    float4 tex = float4(1.0, 1.0, 1.0, 1.0);
    if (d.textures.x != kVfxNoTexture)
        tex = textures[NonUniformResourceIndex(d.textures.x)].SampleLevel(uv, 0);
    const float alpha = tex.a * d.tint.w;
    if (alpha < 0.5) discard;   // the shape: the normal and roughness are replaced inside it

    float3 n = N;
    if (d.textures.y != kVfxNoTexture)
    {
        float3 t = textures[NonUniformResourceIndex(d.textures.y)].SampleLevel(uv, 0).xyz * 2.0 - 1.0;
        t.xy *= d.params.y;
        const float3 T = normalize(mul(d.decalToWorld, float4(1.0, 0.0, 0.0, 0.0)).xyz);
        const float3 B = normalize(mul(d.decalToWorld, float4(0.0, 1.0, 0.0, 0.0)).xyz);
        n = normalize(T * t.x + B * t.y + nd * t.z);
    }

    PsOut o;
    o.albedo = float4(d.tint.xyz * tex.rgb, alpha);
    o.normalRough = float4(encodeOctahedron(n) * 0.5 + 0.5, d.params.x, 0.0);
    return o;
}
