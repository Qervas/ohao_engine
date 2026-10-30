// The offline path tracer (OpenPBR step 1, T3; docs/specs/2026-10-29-openpbr-materials-design.md,
// section 6): the reference the deferred frame is judged by, a correct estimator of
// material/openpbr.slang's BSDF. One path a sample:
//
//   - the camera ray (kRayMaskCamera: scene, light emitters, first-person guns, blended
//     instances) adds what it hits or misses: emission, a light's emitter, the environment;
//   - at every vertex, next-event estimation: one light chosen uniformly (a sphere by its cone of
//     directions, a rectangle by its area, a sun by its direction) and one environment direction
//     by the map's CDF, each behind a shadow ray (kRayMaskScene) and weighted against BSDF
//     sampling by the power heuristic;
//   - the BSDF samples the next direction (material/openpbr.slang); a bounce ray (kRayMaskScene
//     and the light emitters) that meets a light's emitter or leaves the scene adds its radiance
//     weighted against the light or environment sampling that could have found it, so every
//     light is counted once at any roughness (the old near-mirror rule is gone);
//   - Russian roulette from the third vertex; params.w's low 16 bits the bounces (0: direct
//     light only, 1: one bounce, ...).
//
// Light radiances (the deferred frame's, Frank's rulings): a point light of intensity I is a
// sphere of its radius r (at least 1 cm) of radiance I / (pi r^2), windowed by its range; a spot
// the same sphere times its cone toward the shaded point; an area light a one-sided rectangle of
// radiance I / (4 pi r^2); a directional light irradiance I. The emitter meshes RenderScene makes
// for them carry their light's index (InstanceGpu.emitterLight.w).
//
// Outputs as before: the running mean (bindings 1 and 3), the tonemapped image (2, ACES at 0.5),
// OIDN's albedo and normal (6, 7), NRD's and DLSS's guides (19-26).
import shared.layout;
import material.openpbr;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> accumBuffer;
[[vk::binding(2, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> outputImage;
[[vk::binding(3, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> accumHistory;
[[vk::binding(4, 0)]] StructuredBuffer<float> vertices;       // Vertex: 25 floats
[[vk::binding(5, 0)]] StructuredBuffer<uint> indices;         // mesh-local
[[vk::binding(6, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> albedoAOV;
[[vk::binding(7, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> normalAOV;
[[vk::binding(8, 0)]] StructuredBuffer<uint4> meshes;         // vertexOffset, indexOffset, indexCount, triangleSlotOffset
[[vk::binding(9, 0)]] StructuredBuffer<uint> triangleSlots;
[[vk::binding(10, 0)]] StructuredBuffer<float4> materials;    // kMaterialRows a material
[[vk::binding(11, 0)]] StructuredBuffer<float4> lightData;    // header, then 5 rows a light
[[vk::binding(12, 0)]] Sampler2D textures[4096];
[[vk::binding(17, 0)]] StructuredBuffer<float> envMarginal;
[[vk::binding(18, 0)]] StructuredBuffer<float> envConditional;
[[vk::binding(19, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> motionVector;
[[vk::binding(20, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> depthAOV;
[[vk::binding(21, 0)]] [[vk::image_format("r16f")]] RWTexture2D<float> roughnessAOV;
[[vk::binding(22, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> diffuseRadiance;
[[vk::binding(23, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> specularRadiance;
[[vk::binding(24, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> diffuseAlbedoAOV;
[[vk::binding(25, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> specColorAOV;
[[vk::binding(26, 0)]] [[vk::image_format("rgb10_a2")]] RWTexture2D<float4> normalRoughnessAOV;
[[vk::binding(36, 0)]] StructuredBuffer<float4> instances;    // InstanceGpu, kInstanceGpuStride / 16 rows
[[vk::binding(37, 0)]] StructuredBuffer<uint> materialRefs;
[[vk::binding(38, 0)]] StructuredBuffer<float> openpbrTables; // OpenPBRTables (kPbrTableFloats)

// The push constants (PTPushConstants, 256 bytes): matrices as their four columns.
struct PushConstants
{
    float4 invView[4];
    float4 invProj[4];
    float4 prevViewProj[4];
    uint4 params;    // x width, y height, z sample index, w bounces (low 16)
    uint4 control;   // x flags, y frames already in the running mean, z motion vectors valid, w the environment CDF's width
    float4 tuning;   // x firefly clamp, y the environment CDF's height
    float4 jitter;   // xy NRD's sub-pixel offset
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

static const uint kFlagAOVs = 1u;
static const uint kFlagFireflyClamp = 4u;
static const uint kFlagAccumulateAOVs = 8u;
static const uint kFlagDLSSRR = 16u;
static const float kPi = 3.14159265358979;
static const float kTwoPi = 6.28318530717959;
static const uint kVertexFloats = 25u;
static const uint kNoTex = 0xFFFFFFFFu;

// ---- the payload ---------------------------------------------------------------------------

static const uint kHit = 1u;
static const uint kHitEmitter = 2u;

// What a ray brings back: a surface's place and its OpenPBR inputs (resolved from its material
// and textures by the closest hit), a light's emitter (its light), or nothing (a miss).
struct PathPayload
{
    float3 position;
    float t;
    float3 geoNormal;      // facing the ray
    uint flags;
    float3 normal;         // shading, facing the ray's side
    uint light;            // an emitter's light index
    float3 baseColor;
    float baseWeight;
    float3 specularColor;
    float specularWeight;
    float3 emission;
    float metalness;
    float diffuseRoughness;
    float specularRoughness;
    float specularIor;
    float pad;
};

// ---- random numbers: one PCG stream a pixel and sample ------------------------------------

uint pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct Rng
{
    uint state;
    [mutating] float next()
    {
        state = pcg(state);
        return float(state >> 8u) * (1.0 / 16777216.0);
    }
};

// ---- geometry helpers ------------------------------------------------------------------------

void basisOf(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;   // Duff et al. 2017
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

float3 toLocal(float3 v, float3 t, float3 b, float3 n) { return float3(dot(v, t), dot(v, b), dot(v, n)); }
float3 toWorld(float3 v, float3 t, float3 b, float3 n) { return t * v.x + b * v.y + n * v.z; }

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float powerHeuristic(float a, float b)
{
    const float a2 = a * a, b2 = b * b;
    return a2 + b2 > 0.0 ? a2 / (a2 + b2) : 0.0;
}

// ---- lights ---------------------------------------------------------------------------------

uint lightCount() { return asuint(lightData[0].x); }
uint envTexture() { return asuint(lightData[0].y); }
float envScale() { const float s = lightData[0].z; return s > 0.0 ? s : 1.0; }
bool alphaRays() { return asuint(lightData[0].w) == 1u; }
// The flags a ray traces with: opaque unless some instance has an alpha material.
uint traceFlags() { return alphaRays() ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE; }

struct Light
{
    uint type;        // 0 point (sphere), 1 directional, 2 spot, 3 area rectangle
    float3 position;  // a rectangle's corner
    float3 radiance;  // colour x intensity
    float3 direction; // the direction the light travels (directional, spot)
    float innerDeg;
    float outerDeg;
    float range;
    float radius;
    float3 edge1;
    float3 edge2;
    float area;
    bool emitter;     // it has an emitter mesh a bounce ray can meet (LightTable::setEmitterLights)
};

Light loadLight(uint i)
{
    const uint b = 1u + i * 5u;
    const float4 p = lightData[b], c = lightData[b + 1u], d = lightData[b + 2u], e = lightData[b + 3u], f = lightData[b + 4u];
    Light l;
    l.type = uint(p.w + 0.5);
    l.position = p.xyz;
    l.radiance = c.rgb * c.w;
    l.direction = normalize(d.xyz);
    l.innerDeg = d.w;
    l.outerDeg = e.w;
    l.edge1 = e.xyz;
    l.edge2 = f.xyz;
    l.area = f.w;
    l.range = f.x;
    l.radius = max(l.type == 2u ? f.y : d.w, 0.01);
    if (l.type == 3u) l.radius = max(l.innerDeg, 0.01);   // an area light's radius is its dirAndParam.w
    l.emitter = l.type == 3u ? e.w > 0.5 : (l.type != 1u && f.z > 0.5);
    return l;
}

// A point or spot light's emitted radiance toward p: its sphere's I / (pi r^2), its range window
// at p, a spot's cone (the deferred frame's attenuation).
float3 sphereRadiance(Light l, float3 p)
{
    const float r = l.radius;
    float3 le = l.radiance / (kPi * r * r);
    const float d = distance(p, l.position);
    const float x = d / l.range;
    const float w = saturate(1.0 - x * x * x * x);
    le *= w * w;
    if (l.type == 2u)
    {
        const float cosAngle = dot(normalize(p - l.position), l.direction);
        const float ci = cos(radians(l.innerDeg)), co = cos(radians(l.outerDeg));
        le *= saturate((cosAngle - co) / max(ci - co, 1e-4));
    }
    return le;
}

// The solid angle density of a sphere's cone of directions seen from p (0 inside it).
float spherePdf(float3 p, float3 c, float r)
{
    const float d2 = dot(c - p, c - p);
    const float sin2 = r * r / d2;
    if (sin2 >= 1.0) return 0.0;
    const float oneMinusCos = sin2 / (1.0 + sqrt(1.0 - sin2));   // 1 - cos without cancellation
    return 1.0 / (kTwoPi * oneMinusCos);
}

struct LightSample
{
    float3 L;        // toward the light
    float dist;      // how far a shadow ray goes
    float3 radiance; // what arrives along L (irradiance for a sun)
    float pdf;       // solid angle density, the choice of light included; 0 for a sun (a delta)
    bool mis;        // a bounce ray can also meet this light: weigh against BSDF sampling
    bool valid;
};

LightSample sampleLight(float3 p, inout Rng rng)
{
    LightSample s;
    s.valid = false;
    s.mis = false;
    s.pdf = 0.0;
    s.dist = 0.0;
    s.L = float3(0.0, 1.0, 0.0);
    s.radiance = float3(0.0);
    const uint n = lightCount();
    if (n == 0u) return s;
    const uint i = min(uint(rng.next() * float(n)), n - 1u);
    const float choose = 1.0 / float(n);
    const Light l = loadLight(i);
    s.mis = l.emitter;
    const float u1 = rng.next(), u2 = rng.next();
    if (l.type == 1u)
    {
        s.L = -l.direction;
        s.dist = 1.0e4;
        s.radiance = l.radiance / choose;   // a delta: no density to weigh
        s.valid = true;
        return s;
    }
    if (l.type == 3u)
    {
        const float3 q = l.position + l.edge1 * u1 + l.edge2 * u2;
        const float3 nl = normalize(cross(l.edge1, l.edge2));
        const float3 toL = q - p;
        const float d2 = dot(toL, toL);
        const float d = sqrt(d2);
        s.L = toL / d;
        const float cosL = dot(-s.L, nl);
        if (cosL <= 0.0 || l.area <= 0.0) return s;
        s.pdf = choose * d2 / (cosL * l.area);
        s.radiance = l.radiance / (4.0 * kPi * l.radius * l.radius);
        s.dist = d - 1e-3;
        s.valid = true;
        return s;
    }
    // A sphere: uniform over its cone of directions.
    const float3 toC = l.position - p;
    const float d2 = dot(toC, toC);
    const float r = l.radius;
    const float sin2 = r * r / d2;
    if (sin2 >= 1.0) return s;
    const float oneMinusCosMax = sin2 / (1.0 + sqrt(1.0 - sin2));
    const float oneMinusCos = u1 * oneMinusCosMax;
    const float cosT = 1.0 - oneMinusCos;
    const float sinT = sqrt(max(0.0, oneMinusCos * (2.0 - oneMinusCos)));
    const float phi = kTwoPi * u2;
    const float d = sqrt(d2);
    const float3 w = toC / d;
    float3 t, b;
    basisOf(w, t, b);
    s.L = normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + w * cosT);
    const float dc = d * cosT;
    s.dist = max(dc - sqrt(max(r * r - d2 + dc * dc, 0.0)), 0.0) - 1e-3;   // to the sphere's near side
    s.pdf = choose / (kTwoPi * oneMinusCosMax);
    s.radiance = sphereRadiance(l, p);
    s.valid = any(s.radiance > 0.0);
    return s;
}

// What a bounce ray from `origin` that met light `i`'s emitter at `hit` (seen from its normal
// `n`) brings back, and the density light sampling had for that direction.
float3 emitterRadiance(uint i, float3 origin, float3 hit, float3 n, float3 dir, out float pdf)
{
    pdf = 0.0;
    if (i >= lightCount()) return float3(0.0);
    const Light l = loadLight(i);
    if (!l.emitter) return float3(0.0);
    const float choose = 1.0 / float(lightCount());
    if (l.type == 3u)
    {
        const float3 nl = normalize(cross(l.edge1, l.edge2));
        const float cosL = dot(-dir, nl);
        if (cosL <= 0.0 || l.area <= 0.0) return float3(0.0);
        const float d2 = dot(hit - origin, hit - origin);
        pdf = choose * d2 / (cosL * l.area);
        return l.radiance / (4.0 * kPi * l.radius * l.radius);
    }
    if (l.type == 1u) return float3(0.0);
    pdf = choose * spherePdf(origin, l.position, l.radius);
    return sphereRadiance(l, origin);
}

// ---- the environment ------------------------------------------------------------------------

float3 envRadiance(float3 dir)
{
    const uint tex = envTexture();
    if (tex >= 4096u) return float3(0.0);
    const float2 uv = float2(atan2(dir.z, dir.x) / kTwoPi + 0.5, 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / kPi);   // row 0 the zenith
    return textures[NonUniformResourceIndex(tex)].SampleLevel(uv, 0.0).rgb * envScale();
}

bool envSampled() { return envTexture() < 4096u && pc.control.w > 0u && pc.tuning.y > 0.0; }

uint searchCdf(StructuredBuffer<float> cdf, uint base, uint n, float u)
{
    uint lo = 0u, hi = n - 1u;
    while (lo < hi)
    {
        const uint mid = (lo + hi) >> 1u;
        if (cdf[base + mid] < u) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

// The density (solid angle) of a direction under the environment's CDF (render/rt/env_cdf's texel
// histogram: piecewise constant in (u, v)).
float envPdf(float3 dir)
{
    if (!envSampled()) return 0.0;
    const uint W = pc.control.w, H = uint(pc.tuning.y);
    const float theta = acos(clamp(dir.y, -1.0, 1.0));
    const float u = atan2(dir.z, dir.x) / kTwoPi + 0.5;
    const uint x = min(uint(u * float(W)), W - 1u);
    const uint y = min(uint(theta / kPi * float(H)), H - 1u);
    const uint row = y * W;
    const float cond = envConditional[row + x] - (x > 0u ? envConditional[row + x - 1u] : 0.0);
    const float marg = envMarginal[y] - (y > 0u ? envMarginal[y - 1u] : 0.0);
    return max(cond * marg * float(W) * float(H) / (kTwoPi * kPi * max(sin(theta), 1e-4)), 0.0);
}

// A direction drawn by the CDF: a texel, then a point uniformly in it.
float3 sampleEnv(inout Rng rng)
{
    const uint W = pc.control.w, H = uint(pc.tuning.y);
    const uint y = searchCdf(envMarginal, 0u, H, rng.next());
    const uint x = searchCdf(envConditional, y * W, W, rng.next());
    const float u = (float(x) + rng.next()) / float(W);
    const float v = (float(y) + rng.next()) / float(H);
    const float phi = (u - 0.5) * kTwoPi;
    const float theta = v * kPi;
    return float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

// ---- tracing --------------------------------------------------------------------------------

PathPayload trace(float3 origin, float3 dir, uint mask, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1e-4;
    ray.TMax = tMax;
    PathPayload p;
    p.flags = 0u;
    TraceRay(scene, traceFlags(), mask, 0, 0, 0, ray, p);
    return p;
}

bool visible(float3 origin, float3 dir, float dist)
{
    if (dist <= 0.0) return false;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1e-4;
    ray.TMax = dist;
    PathPayload p;
    p.flags = kHit;   // the closest hit is skipped: only the miss shader clears it
    TraceRay(scene, traceFlags() | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             kRayMaskScene, 0, 0, 0, ray, p);
    return (p.flags & kHit) == 0u;
}

MaterialInputs inputsOf(PathPayload p)
{
    MaterialInputs m;
    m.baseColor = p.baseColor;
    m.baseWeight = p.baseWeight;
    m.baseMetalness = p.metalness;
    m.baseDiffuseRoughness = p.diffuseRoughness;
    m.specularWeight = p.specularWeight;
    m.specularColor = p.specularColor;
    m.specularRoughness = p.specularRoughness;
    m.specularIor = p.specularIor;
    return m;
}

float4 nrdPackNormalRoughness(float3 n, float roughness)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float3 r;
    r.y = n.y * 0.5 + 0.5;
    r.x = n.x * 0.5 + r.y;
    r.y -= n.x * 0.5;
    roughness = max(roughness, 1.5 / 512.0);
    r.z = (n.z < 0.0 ? -roughness : roughness) * 0.5 + 0.5;
    return float4(r, 0.0);
}

float4 nrdPackRadianceHitDist(float3 radiance, float hitDist, float viewZ, float roughness)
{
    const float smc = saturate(roughness * roughness);
    const float f = (3.0 + abs(viewZ) * 0.1) * lerp(20.0, 1.0, smc);
    const float normHd = saturate(max(hitDist, 0.0) / max(f, 1e-6));
    const float3 c = max(radiance, float3(0.0));
    return float4(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)), normHd);
}

float3 aces(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// ---- ray generation -------------------------------------------------------------------------

[shader("raygeneration")]
void raygen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 size = pc.params.xy;
    const uint bounces = pc.params.w & 0xFFFFu;
    const uint flags = pc.control.x;
    const bool accumulateAOVs = (flags & kFlagAccumulateAOVs) != 0u;
    const bool blendAOVs = accumulateAOVs && pc.control.y > 0u;
    Rng rng;
    rng.state = pcg(pixel.x ^ pcg(pixel.y ^ pcg(pc.params.z * 0x9E3779B9u + 0x7F4A7C15u)));

    OpenPBRTables tables;
    tables.data = openpbrTables;

    // The camera ray through a jittered point of the pixel.
    const float2 jit = float2(rng.next(), rng.next());
    const float2 uv = (float2(pixel) + jit + pc.jitter.xy) / float2(size);
    const float2 ndc = uv * 2.0 - 1.0;
    const float3 camPos = pc.invView[3].xyz;
    const float3 fwd = -pc.invView[2].xyz;
    const float3 right = pc.invView[0].xyz;
    const float3 up = pc.invView[1].xyz;
    const float tanY = abs(pc.invProj[1].y);
    const float tanX = tanY * float(size.x) / float(size.y);
    float3 dir = normalize(fwd + right * (ndc.x * tanX) - up * (ndc.y * tanY));
    float3 origin = camPos;

    float3 radiance = float3(0.0);
    float3 diffuseOut = float3(0.0), specularOut = float3(0.0);
    float3 throughput = float3(1.0);
    float lastPdf = 0.0;   // the BSDF density the last bounce ray was drawn with (0: the camera ray)
    uint firstLobe = 0u;   // the primary hit's sampled lobe (NRD's split): 0 specular, 1 diffuse
    float firstHitT = -1.0, secondHitT = -1.0;
    float viewZ = 1e30, firstRough = 1.0;
    float3 diffAlbedo = float3(0.0), specColor = float3(0.0);

    for (uint depth = 0u; ; ++depth)
    {
        const PathPayload hit = trace(origin, dir, depth == 0u ? kRayMaskCamera : (kRayMaskScene | kTlasMaskEmitter), 1.0e4);
        float3 add = float3(0.0);
        bool stop = false;
        if ((hit.flags & kHit) == 0u)
        {
            // The environment, weighed against its sampling at the last vertex.
            const float3 le = envRadiance(dir);
            const float w = depth == 0u ? 1.0 : powerHeuristic(lastPdf, envPdf(dir));
            add = throughput * le * w;
            stop = true;
        }
        else if ((hit.flags & kHitEmitter) != 0u)
        {
            // The camera sees the emitter mesh as the G-buffer draws it; a bounce ray takes the
            // light's radiance toward its origin, weighed against light sampling.
            if (depth == 0u)
            {
                add = hit.emission;
            }
            else
            {
                float lightPdf;
                const float3 le = emitterRadiance(hit.light, origin, hit.position, hit.geoNormal, dir, lightPdf);
                add = throughput * le * powerHeuristic(lastPdf, lightPdf);
            }
            stop = true;
        }
        else
        {
            add = throughput * hit.emission;   // emissive surfaces: found by BSDF sampling only
        }
        if (depth == 1u) secondHitT = (hit.flags & kHit) != 0u ? hit.t : 0.0;
        radiance += add;
        if (depth > 0u)
        {
            if (firstLobe == 1u) diffuseOut += add;
            else specularOut += add;
        }
        else if ((hit.flags & kHit) != 0u)
        {
            diffuseOut += add;
        }

        if (depth == 0u)
        {
            // The primary hit's guides.
            firstHitT = (hit.flags & kHit) != 0u ? hit.t : -1.0;
            float2 motion = float2(0.0);
            if (firstHitT > 0.0)
            {
                viewZ = dot(hit.position - camPos, fwd);
                if (pc.control.z != 0u)
                {
                    const float4 prev = pc.prevViewProj[0] * hit.position.x + pc.prevViewProj[1] * hit.position.y +
                                        pc.prevViewProj[2] * hit.position.z + pc.prevViewProj[3];
                    if (prev.w > 0.0)
                    {
                        const float2 prevPix = (prev.xy / prev.w * 0.5 + 0.5) * float2(size);
                        motion = uv * float2(size) - prevPix;
                    }
                }
            }
            motionVector[pixel] = motion;
            depthAOV[pixel] = viewZ;
            if ((hit.flags & kHit) == 0u || (hit.flags & kHitEmitter) != 0u)
            {
                roughnessAOV[pixel] = 1.0;
                if ((flags & kFlagAOVs) != 0u)
                {
                    albedoAOV[pixel] = float4(add, 1.0);
                    normalAOV[pixel] = float4(0.0);
                }
                const float4 zero = float4(0.0);
                diffuseAlbedoAOV[pixel] = blendAOVs ? lerp(diffuseAlbedoAOV[pixel], zero, 1.0 / float(pc.control.y + 1u)) : zero;
                specColorAOV[pixel] = blendAOVs ? lerp(specColorAOV[pixel], zero, 1.0 / float(pc.control.y + 1u)) : zero;
                normalRoughnessAOV[pixel] = blendAOVs ? lerp(normalRoughnessAOV[pixel], zero, 1.0 / float(pc.control.y + 1u)) : zero;
            }
        }
        if (stop || depth > bounces) break;

        // The surface.
        const float3 n = hit.normal;
        float3 t, b;
        basisOf(n, t, b);
        const float3 wo = toLocal(-dir, t, b, n);
        const OpenPBRBase base = OpenPBRBase(inputsOf(hit));
        if (depth == 0u)
        {
            firstRough = clamp(hit.specularRoughness, 0.01, 1.0);
            roughnessAOV[pixel] = firstRough;
            const float3 a = base.albedo(wo, tables);
            if ((flags & kFlagAOVs) != 0u)
            {
                albedoAOV[pixel] = float4(a, 1.0);
                normalAOV[pixel] = (flags & kFlagDLSSRR) != 0u ? float4(n, hit.specularRoughness) : float4(n * 0.5 + 0.5, 1.0);
            }
            diffAlbedo = base.rho * (1.0 - base.metalness);
            specColor = max(a - diffAlbedo, float3(0.0));
            const float k = blendAOVs ? 1.0 / float(pc.control.y + 1u) : 1.0;
            diffuseAlbedoAOV[pixel] = lerp(diffuseAlbedoAOV[pixel], float4(diffAlbedo, 1.0), k);
            specColorAOV[pixel] = lerp(specColorAOV[pixel], float4(specColor, 1.0), k);
            normalRoughnessAOV[pixel] = lerp(normalRoughnessAOV[pixel], nrdPackNormalRoughness(n, firstRough), k);
        }
        // Rays leave from just off the surface, on its geometric side the ray came from.
        const float3 p = hit.position + hit.geoNormal * 1e-3;

        // Next-event estimation: a light, then the environment, each weighed against the BSDF.
        const LightSample ls = sampleLight(hit.position, rng);
        if (ls.valid)
        {
            const float3 wi = toLocal(ls.L, t, b, n);
            const float3 f = base.eval(wo, wi, tables);
            if (any(f > 0.0) && dot(ls.L, hit.geoNormal) > 0.0 && visible(p, ls.L, ls.dist))
            {
                // A sun is a delta (its radiance already over its choice); a light without an
                // emitter mesh only light sampling finds: full weight.
                const float w = ls.pdf > 0.0 ? (ls.mis ? powerHeuristic(ls.pdf, base.pdf(wo, wi, tables)) : 1.0) / ls.pdf : 1.0;
                const float3 c = throughput * f * ls.radiance * w;
                radiance += c;
                if (depth == 0u) { diffuseOut += c; }
                else if (firstLobe == 1u) diffuseOut += c;
                else specularOut += c;
            }
        }
        if (envSampled())
        {
            const float3 L = sampleEnv(rng);
            const float pdfL = envPdf(L);
            const float3 wi = toLocal(L, t, b, n);
            const float3 f = base.eval(wo, wi, tables);
            if (pdfL > 0.0 && any(f > 0.0) && dot(L, hit.geoNormal) > 0.0 && visible(p, L, 1.0e4))
            {
                const float3 c = throughput * f * envRadiance(L) * powerHeuristic(pdfL, base.pdf(wo, wi, tables)) / pdfL;
                radiance += c;
                if (depth == 0u) { diffuseOut += c; }
                else if (firstLobe == 1u) diffuseOut += c;
                else specularOut += c;
            }
        }

        // The next direction.
        const BsdfSample s = base.sample(wo, float3(rng.next(), rng.next(), rng.next()), tables);
        if (!(s.pdf > 0.0)) break;
        const float3 next = toWorld(s.wi, t, b, n);
        if (dot(next, hit.geoNormal) <= 0.0) break;
        if (depth == 0u) firstLobe = s.lobe;
        throughput *= s.weight;
        lastPdf = s.pdf;
        origin = p;
        dir = next;
        if (depth >= 2u)
        {
            const float q = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95);
            if (rng.next() >= q) break;
            throughput /= q;
        }
    }

    if ((flags & kFlagFireflyClamp) != 0u && pc.tuning.x > 0.0)
    {
        const float lum = luminance(radiance);
        if (lum > pc.tuning.x) radiance *= pc.tuning.x / lum;
    }

    // The running mean of every sample since the view last moved (0 frames: this one alone).
    float3 acc = radiance;
    float count = 1.0;
    if (pc.control.y > 0u)
    {
        const float4 prev = accumHistory[pixel];
        count = prev.w + 1.0;
        acc = (prev.rgb * prev.w + radiance) / count;
    }
    accumBuffer[pixel] = float4(acc, count);

    const float k = blendAOVs ? 1.0 / float(pc.control.y + 1u) : 1.0;
    const float3 diffDemod = diffuseOut / max(diffAlbedo, float3(0.03));
    diffuseRadiance[pixel] = lerp(diffuseRadiance[pixel], nrdPackRadianceHitDist(diffDemod, firstLobe == 1u ? secondHitT : 0.0, viewZ, firstRough), k);
    specularRadiance[pixel] = lerp(specularRadiance[pixel], nrdPackRadianceHitDist(specularOut, firstLobe == 0u ? secondHitT : 0.0, viewZ, firstRough), k);

    outputImage[pixel] = float4(pow(aces(acc * 0.5), float3(1.0 / 2.2)), 1.0);
}

// ---- hit shaders ----------------------------------------------------------------------------

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

[shader("closesthit")]
void closestHit(inout PathPayload payload, BuiltInTriangleIntersectionAttributes attr)
{
    const float2 bary = attr.barycentrics;
    const float w = 1.0 - bary.x - bary.y;
    const Corners c = cornersOf(InstanceID(), PrimitiveIndex());
    const float3 dir = WorldRayDirection();
    payload.t = RayTCurrent();
    payload.position = WorldRayOrigin() + dir * payload.t;
    payload.flags = kHit;

    // The geometric normal from the triangle's corners, and the interpolated shading normal,
    // into world space (normals by the inverse transpose), both turned toward the ray.
    const float3x3 w2o = (float3x3)WorldToObject3x4();
    const float3 p0 = vertexF3(c.v0, 0u), p1 = vertexF3(c.v1, 0u), p2 = vertexF3(c.v2, 0u);
    float3 geo = normalize(mul(cross(p1 - p0, p2 - p0), w2o));
    const float3 nObj = vertexF3(c.v0, 6u) * w + vertexF3(c.v1, 6u) * bary.x + vertexF3(c.v2, 6u) * bary.y;
    float3 n = dot(nObj, nObj) > 1e-8 ? normalize(mul(nObj, w2o)) : geo;
    const bool front = dot(geo, dir) < 0.0;   // the side the winding faces (emitters emit from it)
    if (!front) geo = -geo;
    if (dot(n, geo) < 0.0) n = -n;
    payload.geoNormal = geo;

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
        float3 tw = mul((float3x3)ObjectToWorld3x4(), tObj.xyz);
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

    payload.baseColor = baseColor;
    payload.baseWeight = row5.z;
    payload.metalness = saturate(metalness);
    payload.diffuseRoughness = row5.y;
    payload.specularWeight = specularWeight;
    payload.specularColor = specularColor;
    payload.specularRoughness = saturate(roughness);
    payload.specularIor = row5.x;
    payload.emission = emission;
}

[shader("anyhit")]
void anyHit(inout PathPayload payload, BuiltInTriangleIntersectionAttributes attr)
{
    const Corners c = cornersOf(InstanceID(), PrimitiveIndex());
    const uint m = c.material * kMaterialRows;
    const float4 row3 = materials[m + 3u];   // mode, cutoff, opacity, base alpha
    const uint mode = uint(row3.x + 0.5);
    if (mode == kAlphaOpaque) return;
    float alpha = row3.w * row3.z;
    const uint albedoTex = asuint(materials[m].w);
    if (albedoTex < 4096u)
    {
        const float2 bary = attr.barycentrics;
        const float w = 1.0 - bary.x - bary.y;
        const float2 uv = float2(vertexFloat(c.v0, 9u), vertexFloat(c.v0, 10u)) * w +
                          float2(vertexFloat(c.v1, 9u), vertexFloat(c.v1, 10u)) * bary.x +
                          float2(vertexFloat(c.v2, 9u), vertexFloat(c.v2, 10u)) * bary.y;
        alpha *= sampleTex(albedoTex, uv).a;
    }
    if (mode == kAlphaMask)
    {
        if (alpha < row3.y) IgnoreHit();
        return;
    }
    // Blended: stochastically present, by a hash of the ray and the triangle.
    const uint2 launch = DispatchRaysIndex().xy;
    uint h = pcg(launch.x ^ pcg(launch.y ^ pcg(PrimitiveIndex() ^ pcg(InstanceID()))));
    h = pcg(h ^ asuint(WorldRayOrigin().x) ^ pcg(asuint(WorldRayOrigin().y) ^ asuint(WorldRayDirection().z)));
    if (float(h >> 8u) * (1.0 / 16777216.0) >= alpha) IgnoreHit();
}

[shader("miss")]
void miss(inout PathPayload payload)
{
    payload.flags = 0u;
}
