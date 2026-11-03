// The offline path tracer (OpenPBR step 1, T3; docs/specs/2026-10-29-openpbr-materials-design.md,
// section 6): the reference the deferred frame is judged by, a correct estimator of
// material/openpbr.slang's BSDF. One path a sample:
//
//   - the camera ray (kRayMaskCamera: scene, light emitters, first-person guns, blended
//     instances) adds what it hits or misses: emission, a light's emitter, the environment;
//   - at every vertex, next-event estimation: one light chosen uniformly (a sphere by its cone of
//     directions, a rectangle by its area, a sun by its direction) and one environment direction
//     by the map's CDF, each behind a shadow ray (kPathMask) and weighted against BSDF
//     sampling by the power heuristic;
//   - the BSDF samples the next direction (material/openpbr.slang); a bounce ray (kPathMask
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
// The realtime profile ends a path in the radiance cache (rt/radiance_cache.slang, kFlagCache) at
// its second or later vertex; cacheUpdate fills the cache. The offline profile never reads it.
//
// Outputs as before: the running mean (bindings 1 and 3), the displayed image (2, display/display.slang),
// OIDN's albedo and normal (6, 7), NRD's and DLSS's guides (19-26).
import shared.layout;
import material.openpbr;
import material.material_layers;
import sampling.sobol;
import rt.radiance_cache;
import rt.path_guiding;
import sampling.vmf;
import rt.lights;
import display.display;

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
[[vk::binding(13, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> emissionAOV;   // the camera ray's emission
[[vk::binding(17, 0)]] StructuredBuffer<float> envCdf;   // the marginal CDF (H), then each row's conditional (W x H)
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
[[vk::binding(39, 0)]] RWStructuredBuffer<uint> cacheKeys;      // the radiance cache (rt/radiance_cache.slang)
[[vk::binding(40, 0)]] RWStructuredBuffer<uint4> cacheAccum;
[[vk::binding(41, 0)]] RWStructuredBuffer<uint4> cacheResolved;
// Path guiding (rt/path_guiding.slang): the states frame N-1 left, this frame's proposals and each
// pixel's sample radiance and proposal count (rt/path_guiding_update.slang reads them after).
[[vk::binding(46, 0)]] StructuredBuffer<GuideState> guideStates;
[[vk::binding(49, 0)]] RWStructuredBuffer<GuideProposal> guideProposals;
[[vk::binding(50, 0)]] RWStructuredBuffer<float4> guidePixel;

// The push constants (PTPushConstants, 256 bytes): matrices as their four columns.
struct PushConstants
{
    float4 invView[4];
    float4 invProj[4];
    float4 prevViewProj[4];
    uint4 params;    // x width, y height, z sample index, w bounces (low 16) and guided vertices (high 16, 0: all)
    uint4 control;   // x flags, y frames already in the running mean, z motion vectors valid, w the environment map's width
    float4 tuning;   // x firefly clamp, y the environment map's height, z the display exposure, w the display curve (bits)
    float4 jitter;   // xy NRD's sub-pixel offset, z path guiding's share of a guided bounce, w its lobes' prior (samples)
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

static const uint kFlagAOVs = 1u;
static const uint kFlagFireflyClamp = 4u;
static const uint kFlagAccumulateAOVs = 8u;
static const uint kFlagDLSSRR = 16u;
static const uint kFlagCache = 32u;   // realtime: paths end in the radiance cache
static const uint kFlagGuiding = 64u; // path guiding (sub-project 23): bounces drawn from the BSDF and learned lobes
static const uint kFlagGuideDebug = 128u;   // OHAO_GUIDE_DEBUG: the first hit's lobes instead of light
// A guided bounce's coin, lobe pick and vMF direction: Sobol group kGuideGroup + depth (outside the
// bounce groups 1 + 3d to 3 + 3d, whose sequences stay as they are).
static const uint kGuideGroup = 0x4000u;
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

// ---- random numbers -------------------------------------------------------------------------
// The path's draws are Owen-scrambled Sobol (sampling/sobol.slang): group 0 the camera's jitter;
// bounce d's light choice and point in group 1 + 3d, its environment sample in 2 + 3d, its BSDF
// sample and Russian roulette in 3 + 3d. PCG hashes the pixel's seed and the any-hit's coin.

uint pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// ---- geometry helpers ------------------------------------------------------------------------

float3 toLocal(float3 v, float3 t, float3 b, float3 n) { return float3(dot(v, t), dot(v, b), dot(v, n)); }
float3 toWorld(float3 v, float3 t, float3 b, float3 n) { return t * v.x + b * v.y + n * v.z; }

float powerHeuristic(float a, float b)
{
    const float a2 = a * a, b2 = b * b;
    return a2 + b2 > 0.0 ? a2 / (a2 + b2) : 0.0;
}

// A bounce ray that met an emissive triangle: its weight against the light sampling that could have
// drawn the same point (the camera ray's hit, depth 0, is not weighed).
float emissiveHitWeight(PathPayload hit, float3 origin, float lastPdf, uint depth)
{
    if (depth == 0u || (hit.flags & kHitEmissive) == 0u) return 1.0;
    return powerHeuristic(lastPdf, emissivePdf(hit.light, origin, hit.position));
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

uint searchCdf(uint base, uint n, float u)
{
    uint lo = 0u, hi = n - 1u;
    while (lo < hi)
    {
        const uint mid = (lo + hi) >> 1u;
        if (envCdf[base + mid] < u) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

// Texel (x, y)'s probability (render/rt/env_cdf: luminance x sin(theta), piecewise constant in
// (u, v)) as a density in solid angle at polar angle theta: the texel covers (2 pi / W) x (pi / H)
// of (phi, theta), times sin(theta).
float envDensity(uint x, uint y, float theta)
{
    const uint W = pc.control.w, H = uint(pc.tuning.y);
    const uint row = H + y * W;
    const float cond = envCdf[row + x] - (x > 0u ? envCdf[row + x - 1u] : 0.0);
    const float marg = envCdf[y] - (y > 0u ? envCdf[y - 1u] : 0.0);
    return max(cond * marg * float(W) * float(H) / (kTwoPi * kPi * max(sin(theta), 1e-4)), 0.0);
}

// The density (solid angle) of a direction under the environment's sampling.
float envPdf(float3 dir)
{
    if (!envSampled()) return 0.0;
    const uint W = pc.control.w, H = uint(pc.tuning.y);
    const float theta = acos(clamp(dir.y, -1.0, 1.0));
    const float u = atan2(dir.z, dir.x) / kTwoPi + 0.5;
    const uint x = min(uint(u * float(W)), W - 1u);
    const uint y = min(uint(theta / kPi * float(H)), H - 1u);
    return envDensity(x, y, theta);
}

// A direction drawn by inverting the CDFs (a row by the marginal, a texel by its row's
// conditional), then a point uniformly in the texel: a continuous warp, so the Sobol points stay
// stratified on the sphere. `pdf` its density.
float3 sampleEnv(inout Sampler rng, out float pdf)
{
    const uint W = pc.control.w, H = uint(pc.tuning.y);
    const uint y = searchCdf(0u, H, rng.next());
    const uint x = searchCdf(H + y * W, W, rng.next());
    const float u = (float(x) + rng.next()) / float(W);
    const float v = (float(y) + rng.next()) / float(H);
    const float phi = (u - 0.5) * kTwoPi;
    const float theta = v * kPi;
    pdf = envDensity(x, y, theta);
    return float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

// sampleEnv from four uniforms of another stream (the subsurface walk's exit).
float3 sampleEnvAt(float4 r, out float pdf)
{
    const uint W = pc.control.w, H = uint(pc.tuning.y);
    const uint y = searchCdf(0u, H, r.x);
    const uint x = searchCdf(H + y * W, W, r.y);
    const float u = (float(x) + r.z) / float(W);
    const float v = (float(y) + r.w) / float(H);
    const float phi = (u - 0.5) * kTwoPi;
    const float theta = v * kPi;
    pdf = envDensity(x, y, theta);
    return float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

// The flags a ray traces with: opaque unless some instance has an alpha material.
uint traceFlags() { return alphaRays() ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE; }

// ---- tracing --------------------------------------------------------------------------------

// tMin 0 for a subsurface walk's step: it starts inside a volume, not on a surface, and a 1e-4 start
// let a step from a scattering event within 0.1 mm of the surface step through it unseen and the walk
// escape (a dense white volume lost 1.4%).
PathPayload trace(float3 origin, float3 dir, uint mask, float tMax, float tMin = 1e-4)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = tMin;
    ray.TMax = tMax;
    PathPayload p;
    p.flags = 0u;
    TraceRay(scene, traceFlags(), mask, 0, 0, 0, ray, p);
    return p;
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

// Whether the segment is unblocked, and what it carries through panes (T: 1 without any). A shadow
// ray's payload carries the product of the panes' straight transmission in `emission`, which anyHit
// multiplies (the order does not matter); a pane that lets nothing through is a hit. Kept to one trace:
// a loop here, inlined at every light sample, made the whole tracer 8x slower on scenes without glass.
bool visible(float3 origin, float3 dir, float dist, out float3 T)
{
    T = float3(0.0);
    if (dist <= 0.0) return false;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1e-4;
    ray.TMax = dist;
    PathPayload p;
    p.flags = kHit | kShadowRay;   // the closest hit is skipped: only the miss shader clears kHit
    p.emission = float3(1.0);
    TraceRay(scene, traceFlags() | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             kPathMask, 0, 0, 0, ray, p);
    if ((p.flags & kHit) != 0u) return false;
    T = p.emission;
    return true;
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

// ---- the subsurface walk (OpenPBR step 4) --------------------------------------------------
// Chiang, Kutz & Burley 2016: light enters (lobe 5) and walks the volume under the surface,
// sigma_t = 1 / (subsurface_radius x subsurface_radius_scale), its single-scatter albedo from
// subsurface_color by van de Hulst's inversion, Henyey-Greenstein scattering. Every walk carries all
// three channels (spectral MIS): each step draws its distance on a channel chosen in proportion to
// the walk's throughput, and each channel is weighed by its own density over the mixture's
// (Cycles' scheme). With that choice the channels' summed throughput never grows (a scattering
// multiplies it by at most the largest single-scatter albedo, leaving by exactly 1), so skin's
// free paths, 4x apart, cannot build the long tail that uniform choice did. A walk that reaches
// kWalkSteps leaves along its direction at the surface it meets (a dense near-white volume would
// otherwise lose the energy of its longest walks: the exit point is biased, the energy is not).
// Meeting the surface is leaving it (ruling O16). Its own PCG stream: a walk takes up to
// kWalkSteps steps, more than any Sobol group holds. The walk is a mode of raygen's path loop (its
// steps are the loop's trace, its exit's light sampling the loop's): inlined beside the loop's own
// traces, a walk of its own made the whole tracer 3.7x slower on scenes without subsurface.
static const uint kWalkSteps = 1024u;

struct WalkRng
{
    uint state;
    [mutating] float next()
    {
        state = pcg(state);
        return float(state >> 8u) * (1.0 / 16777216.0);
    }
};

// A direction about d by Henyey-Greenstein with anisotropy g.
float3 sampleHG(float3 d, float g, float u1, float u2)
{
    float c;
    if (abs(g) < 1e-3) c = 1.0 - 2.0 * u1;
    else
    {
        const float s = (1.0 - g * g) / (1.0 - g + 2.0 * g * u1);
        c = clamp((1.0 + g * g - s * s) / (2.0 * g), -1.0, 1.0);
    }
    const float sinT = sqrt(max(0.0, 1.0 - c * c));
    const float phi = 2.0 * kPi * u2;
    float3 tt, bb;
    basisOf(d, tt, bb);
    return normalize(tt * (sinT * cos(phi)) + bb * (sinT * sin(phi)) + d * c);
}

// Leaving at a surface hit h (met from inside): pos just outside it, dir cosine-weighted about the
// outward shading normal.
void walkExit(PathPayload h, inout float3 pos, inout float3 dir, out float3 shade, inout WalkRng rng)
{
    const float3 out = -h.geoNormal;   // geoNormal faces the ray, which came from inside
    shade = dot(h.normal, out) > 0.0 ? h.normal : -h.normal;
    float3 tt, bb;
    basisOf(shade, tt, bb);
    const float u1 = rng.next(), u2 = rng.next();
    const float r = sqrt(u1), phi = 2.0 * kPi * u2;
    dir = normalize(tt * (r * cos(phi)) + bb * (r * sin(phi)) + shade * sqrt(max(1.0 - u1, 0.0)));
    if (dot(dir, out) <= 0.0) dir = out;
    pos = h.position + out * 1e-3;
}

static const uint kNoWalk = 0xFFFFFFFFu;

// A walked material's medium, from its rows 15-16.
struct WalkMedium
{
    float3 sigmaT;
    float3 albedo;   // single scatter
    float g;
};

WalkMedium walkMedium(uint material)
{
    const uint mr = material * kMaterialRows;
    const float4 r15 = materials[mr + kSubsurfaceRowFirst], r16 = materials[mr + kSubsurfaceRowFirst + 1u];
    WalkMedium m;
    m.sigmaT = 1.0 / max(r15.w * r16.rgb, float3(1e-5));
    m.albedo = subsurfaceSingleScatter(r15.rgb);
    m.g = clamp(r16.w, -0.95, 0.95);
    return m;
}

// A walk step's distance, on a channel drawn by throughput (p_j = w_j / sum w).
float walkDraw(inout WalkRng rng, float3 weight, float3 sigmaT)
{
    const float3 pj = weight / (weight.r + weight.g + weight.b);
    const float uc = rng.next();
    const uint c = uc < pj.r ? 0u : uc < pj.r + pj.g ? 1u : 2u;
    return -log(max(1.0 - rng.next(), 1e-7)) / sigmaT[c];
}

// Dispersion (OpenPBR step 3, ruling O12): a path's first dispersive refraction draws one of RGB at
// these wavelengths (nm), weighs it by 3 and keeps it; its IOR is dispersedIor's there.
static const float3 kDispersionLambda = float3(600.0, 550.0, 450.0);

// The shading frame: an anisotropic material's tangent, else any basis of n.
void frameOf(PathPayload p, out float3 t, out float3 b)
{
    if (dot(p.tangent, p.tangent) > 0.0)
    {
        t = p.tangent;
        b = cross(p.normal, t);
    }
    else
    {
        basisOf(p.normal, t, b);
    }
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
    Sampler rng = Sampler(pc.params.z, pcg(pixel.x ^ pcg(pixel.y ^ 0x7F4A7C15u)));

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
    float3 misOrigin = origin;   // the vertex that density is at (a pane's null crossing keeps it)
    uint firstLobe = 0u;   // the primary hit's sampled lobe (NRD's split): 0 specular, 1 diffuse
    float firstHitT = -1.0, secondHitT = -1.0;
    float viewZ = 1e30, firstRough = 1.0;
    float3 diffAlbedo = float3(0.0), specColor = float3(0.0);
    uint lastLobe = 0u;       // the last vertex's sampled lobe and its roughness (the cache's cone rule)
    float lastRough = 0.0;
    // Inside a transmissive solid (OpenPBR step 3): its absorption per metre (transmission_color at
    // transmission_depth, Beer-Lambert), applied to every segment until the path refracts out.
    bool insideVolume = false;
    float3 sigmaA = float3(0.0);
    int channel = -1;          // dispersion's wavelength once drawn (0 R, 1 G, 2 B)
    uint skips = 0u;           // culled faces the camera ray passed, and how far it went past them
    float skipped = 0.0;
    uint crossings = 0u;       // glass events so far: they do not count as bounces
    // The subsurface walk (lobe 5) in progress: the loop's trace is its step (from tMin 0, up to the
    // drawn distance, kRayMaskScene), and the step that meets the surface is its exit vertex, shaded
    // Lambertian by the loop's light sampling. Its iterations do not count as bounces.
    // Its state is kept small, since what raygen holds across a trace is spilled by every pixel
    // (each word of it showed in pt.trace on scenes without subsurface): the medium is read back from
    // the material's rows at each step, the channel probabilities from the weight, and the rest lives
    // in the last vertex's MIS state, which means nothing during a walk and which its exit sets:
    // misOrigin holds the walk's weight, lastPdf its step's distance, lastLobe the walked material.
    uint walkSteps = kNoWalk;   // scattering events so far (kWalkSteps: the cap's ray)
    // Path guiding: the proposals written (at most kGuideTrainVertices) and the one whose ray the
    // next trace is (its end point is written after it).
    const bool guiding = (flags & kFlagGuiding) != 0u;
    const uint pixelIndex = pixel.y * size.x + pixel.x;
    uint guideProposed = 0u;
    uint guidePending = kGuideNoSlot;
    float3 guideDebugColor = float3(0.0);
    // Russian roulette's throughput is the path's as the BSDF alone would have drawn it: a guided
    // bounce toward a small bright source has a large density, so a small weight, and roulette by
    // the true throughput killed exactly the paths guiding aimed well (guided error fell 2.4x at two
    // bounces and nothing from the third, where roulette starts). Any survival probability keeps the
    // estimate unbiased; this one never kills more than the true throughput's would (a bounce the
    // BSDF drew away from every lobe weighs up to 1 / (1 - share) its BSDF weight, and roulette by
    // the BSDF's throughput alone grew such paths 2x a bounce: fireflies). 1 at every unguided vertex.
    float rrRatio = 1.0;
    WalkRng wr;
    wr.state = 0u;
#define walkWeight misOrigin
#define walkDist lastPdf
#define walkMaterial lastLobe

    for (uint depth = 0u; ; ++depth)
    {
        // The path's one trace (every TraceRay site inlined into raygen costs every pixel).
        const bool walking = walkSteps != kNoWalk;
        const uint mask = walking ? kRayMaskScene : depth == 0u ? kRayMaskCamera : (kPathMask | kTlasMaskEmitter);
        const PathPayload hit = trace(origin, dir, mask, walking ? walkDist : 1.0e4, walking ? 0.0 : 1e-4);
        if (guidePending != kGuideNoSlot)
        {
            guideProposals[guidePending].xnext = (hit.flags & kHit) != 0u ? hit.position : origin + dir * kGuideFar;
            guideProposals[guidePending].flags = kGuideHasNext;
        }
        bool walkExitVertex = false;
        float3 exitN = float3(0.0);
        if (walking)
        {
            const WalkMedium m = walkMedium(walkMaterial);
            const float3 pj = walkWeight / (walkWeight.r + walkWeight.g + walkWeight.b);
            if ((hit.flags & kHit) == 0u)
            {
                if (walkSteps == kWalkSteps) break;   // the cap's ray left without meeting the surface
                // A scattering event at walkDist: sigma_s T over the mixture's density sum_j p_j sigma_t,j T_j.
                const float3 tr = exp(-m.sigmaT * walkDist);
                walkWeight *= m.albedo * m.sigmaT * tr / max(dot(pj, m.sigmaT * tr), 1e-30);
                origin += dir * walkDist;
                dir = sampleHG(dir, m.g, wr.next(), wr.next());
                // Russian roulette only once the walk has lost weight (absorbed): a white walk is never
                // cut short (a 1% cut a step kept a 500-step walk once in 150, at 150 times its weight,
                // which 16 samples a pixel never see: a dense white volume read 0.94).
                const float q = max(walkWeight.r, max(walkWeight.g, walkWeight.b));
                if (q < 1.0)
                {
                    if (wr.next() >= q) break;
                    walkWeight /= q;
                }
                // The cap: leave along dir where it meets the surface.
                walkDist = 1.0e4;
                if (++walkSteps < kWalkSteps) walkDist = walkDraw(wr, walkWeight, m.sigmaT);
                --depth;
                continue;
            }
            // Out: no event before the surface, each channel's transmittance over the mixture's.
            if (walkSteps < kWalkSteps)
            {
                const float3 tr = exp(-m.sigmaT * length(hit.position - origin));
                walkWeight *= tr / max(dot(pj, tr), 1e-30);
            }
            walkExit(hit, origin, dir, exitN, wr);
            throughput *= walkWeight;
            walkSteps = kNoWalk;
            walkExitVertex = true;
        }
        // The walk's exit counts no emission and is no bounce: it goes straight to light sampling.
        if (!walkExitVertex)
        {
            // The camera sees no back face of a one-sided material, as the G-buffer culls it (glTF's
            // doubleSided false): it goes on to what the raster draws behind. Other rays meet both sides
            // (one-sided geometry still shadows and reflects light, as in the deferred frame's rays).
            if (depth == 0u && (hit.flags & kHitCulled) != 0u && skips < kCullSkips)
            {
                ++skips;
                skipped += hit.t;
                origin = hit.position;
                --depth;   // the camera ray again (the loop's ++depth brings it back to 0)
                continue;
            }
            if (insideVolume) throughput *= exp(-sigmaA * ((hit.flags & kHit) != 0u ? hit.t : 1.0e4));
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
                    const float3 le = emitterRadiance(hit.light, misOrigin, hit.position, hit.geoNormal, dir, lightPdf);
                    add = throughput * le * powerHeuristic(lastPdf, lightPdf);
                }
                stop = true;
            }
            else
            {
                add = throughput * hit.emission * emissiveHitWeight(hit, misOrigin, lastPdf, depth);   // weighed against light sampling
            }
            if (depth == 1u) secondHitT = (hit.flags & kHit) != 0u ? hit.t : 0.0;
            radiance += add;
            if (guidePending != kGuideNoSlot)
            {
                // The light guiding learns is what this hit reflects: what it emits (a light's mesh,
                // an emissive triangle, the sky) light sampling already finds at the last vertex, and
                // learning it too moved that light from light sampling to the lobes through MIS.
                guideProposals[guidePending].rk = radiance;
                guidePending = kGuideNoSlot;
            }
            if (depth > 0u)
            {
                if (firstLobe == 1u) diffuseOut += add;
                else specularOut += add;
            }
            else
            {
                // What the camera ray meets emits: never demodulated, never denoised (nrd_compose adds it).
                emissionAOV[pixel] = lerp(emissionAOV[pixel], float4(add, 1.0), blendAOVs ? 1.0 / float(pc.control.y + 1u) : 1.0);
            }

            if (depth == 0u)
            {
                // The primary hit's guides.
                firstHitT = (hit.flags & kHit) != 0u ? hit.t + skipped : -1.0;
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
            // The radiance cache (realtime): a surface past the first vertex gives the light it reflects
            // and the path ends there, if the segment reaching it is at least its voxel long and the
            // lobe that drew it spreads over the voxel (a diffuse one, or roughness^2 x length >= voxel:
            // a sharp reflection keeps tracing). The query point is jittered within the voxel's face
            // (Binder 2019) so no voxel shows as a block.
            // Never at glass: the cache's update paths stop where a surface refracts, so a cell there
            // would hold its reflection alone.
            if (!stop && depth > 0u && (flags & kFlagCache) != 0u &&
                !(materials[hit.material * kMaterialRows + kTransmissionRowFirst].w > 0.0))
            {
                const float voxel = cacheVoxelSizeAt(hit.position, camPos);
                if (hit.t >= voxel && (lastLobe == 1u || lastRough * lastRough * hit.t >= voxel))
                {
                    float3 jt, jb;
                    basisOf(hit.geoNormal, jt, jb);
                    const uint h = pcg(pixel.x ^ pcg(pixel.y ^ pcg(pc.params.z ^ (depth * 0x9E3779B9u))));
                    const float2 j = (float2(float(h & 0xFFFFu), float(h >> 16u)) / 65536.0 - 0.5) * voxel;
                    float3 cached;
                    if (cacheLookup(cacheKeys, cacheResolved, cacheCell(hit.position + jt * j.x + jb * j.y, hit.geoNormal, camPos),
                                    cached))
                    {
                        const float3 c = throughput * cached;
                        radiance += c;
                        if (firstLobe == 1u) diffuseOut += c;
                        else specularOut += c;
                        break;
                    }
                }
            }
            // Glass has its own budget (Cycles' transmission bounces): a ray through a ball crosses
            // twice, and inside rough glass it reflects off the inner wall again and again; counted as
            // bounces, the realtime profile's 2 ended it inside (the realtime furnace's rough glass rim
            // read 0.62).
            if (stop || depth - crossings > bounces || crossings > kCrossings) break;
        }

        // The surface. A transmissive solid met from inside (a back face) is shaded in its outward
        // frame, so the module sees wo below it and refracts at 1 / eta.
        MaterialInputs mi = inputsOf(hit);
        const bool transmissive = mi.transmissionWeight > 0.0 && mi.baseMetalness < 1.0;
        const bool fromInside = transmissive && !mi.thinWalled && (hit.flags & kHitBack) != 0u;
        const float3 n = walkExitVertex ? exitN : fromInside ? -hit.normal : hit.normal;
        float3 t, b;
        if (dot(hit.tangent, hit.tangent) > 0.0)
        {
            t = hit.tangent;
            b = cross(n, t);
        }
        else
        {
            basisOf(n, t, b);
        }
        const float3 wo = toLocal(-dir, t, b, n);
        // Dispersion: the IOR at the path's wavelength (drawn at the first dispersive glass).
        const float4 row13 = materials[hit.material * kMaterialRows + kTransmissionRowFirst + 1u];
        if (!walkExitVertex && transmissive && row13.z > 0.0)
        {
            if (channel < 0)
            {
                const uint h = pcg(pixel.x ^ pcg(pixel.y ^ pcg(pc.params.z ^ 0x51ED270Bu)));
                channel = int(min(float(h >> 8u) * (3.0 / 16777216.0), 2.0));
                throughput *= float3(channel == 0 ? 3.0 : 0.0, channel == 1 ? 3.0 : 0.0, channel == 2 ? 3.0 : 0.0);
            }
            mi.specularIor = dispersedIor(mi.specularIor, row13.w, row13.z, kDispersionLambda[channel]);
        }
        const OpenPBRSurface base = OpenPBRSurface(mi);
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
            diffAlbedo = base.base.rho * (1.0 - base.base.metalness);
            specColor = max(a - diffAlbedo, float3(0.0));
            const float k = blendAOVs ? 1.0 / float(pc.control.y + 1u) : 1.0;
            diffuseAlbedoAOV[pixel] = lerp(diffuseAlbedoAOV[pixel], float4(diffAlbedo, 1.0), k);
            specColorAOV[pixel] = lerp(specColorAOV[pixel], float4(specColor, 1.0), k);
            normalRoughnessAOV[pixel] = lerp(normalRoughnessAOV[pixel], nrdPackNormalRoughness(n, firstRough), k);
        }
        // Rays leave from just off the surface, on its geometric side the ray came from. A walk's exit
        // (ruling O16) is a Lambertian vertex about its outward shading normal, f = 1 / pi, already
        // just outside; its draws come from the walk's stream.
        const float3 p = walkExitVertex ? origin : hit.position + hit.geoNormal * 1e-3;
        const float3 ng = walkExitVertex ? n : hit.geoNormal;
        // Path guiding (sub-project 23, ruling G4): at an opaque surface's reflection, the lobes the
        // grid holds near here; the bounce and light sampling's weights use their mixture's density.
        GuideMixture gm;
        gm.g = 0.0;
        gm.total = 0.0;
        // A guidable vertex proposes what it learned even while the grid holds nothing here (its
        // bounce is then the BSDF's alone): that is how the grid fills.
        const uint guideDepths = pc.params.w >> 16u;   // vertices guided from the camera's (0: every one)
        const bool guidable = guiding && !walkExitVertex && !transmissive && !mi.thinWalled && !(mi.subsurfaceWeight > 0.0) &&
                              (guideDepths == 0u || depth < guideDepths);
        if (guidable)
            gm = guideRead(guideStates, hit.position, hit.geoNormal, camPos,
                           pcg(pixel.x ^ pcg(pixel.y ^ pcg(pc.params.z ^ pcg(depth ^ 0x6A09E667u)))), pc.jitter.w,
                           pc.jitter.z);
        if (depth == 0u && (flags & kFlagGuideDebug) != 0u)
        {
            // What the grid holds at the first hit, shown instead of the light (the path still runs
            // and trains): red the share of states read, green the strongest lobe's sharpness
            // (kappa / (kappa + 10)), blue its weight (w / (w + 1)).
            // OHAO_GUIDE_DEBUG=2: the first hit's position; 3: the strongest lobe's mean direction.
            float valid = 0.0, best = 0.0, sharp = 0.0;
            float3 bestMu = float3(0.0);
            if (guidable)
                [unroll] for (uint i = 0u; i < kGuideCandidates; ++i)
                    if (gm.w[i] > 0.0)
                    {
                        valid += 1.0 / float(kGuideCandidates);
                        if (gm.w[i] > best) { best = gm.w[i]; sharp = gm.kappa[i] / (gm.kappa[i] + 10.0); bestMu = gm.mu[i]; }
                    }
            const uint mode = pc.control.x >> 8u & 3u;
            guideDebugColor = mode == 2u ? hit.position : mode == 3u ? bestMu : float3(valid, sharp, best / (best + 1.0));
        }

        // Next-event estimation: a light, then the environment, each weighed against the BSDF.
        float lu0, lu1, lu2;
        if (walkExitVertex)
        {
            lu0 = wr.next();
            lu1 = wr.next();
            lu2 = wr.next();
        }
        else
        {
            rng.group(1u + 3u * depth);
            lu0 = rng.next();
            lu1 = rng.next();
            lu2 = rng.next();
        }
        const LightSample ls = sampleLight(walkExitVertex ? origin : hit.position, p, lu0, lu1, lu2);
        if (ls.valid)
        {
            const float3 wi = toLocal(ls.L, t, b, n);
            float3 fs = float3(0.0), fd;
            if (walkExitVertex) fd = float3(max(dot(ls.L, n), 0.0) * (1.0 / kPi));
            else base.evalLobes(wo, wi, tables, fs, fd);
            const float3 f = fs + fd;
            float3 vis1;
            if (any(f > 0.0) && dot(ls.L, ng) > 0.0 && visible(p, ls.L, ls.dist, vis1))
            {
                // A sun is a delta (its radiance already over its choice); a light without an
                // emitter mesh only light sampling finds: full weight.
                float w = 1.0;
                if (ls.pdf > 0.0)
                {
                    float bsdfPdf = fd.x;
                    if (ls.mis && !walkExitVertex) bsdfPdf = guidePdf(gm, ls.L, base.pdf(wo, wi, tables));
                    w = (ls.mis ? powerHeuristic(ls.pdf, bsdfPdf) : 1.0) / ls.pdf;
                }
                const float3 le = ls.radiance * vis1 * w;
                const float3 c = throughput * f * le;
                radiance += c;
                // The first hit's direct light by lobe (NRD's split); later ones by the first lobe.
                if (depth == 0u) { diffuseOut += throughput * fd * le; specularOut += throughput * fs * le; }
                else if (firstLobe == 1u) diffuseOut += c;
                else specularOut += c;
            }
        }
        if (envSampled())
        {
            float pdfL;
            float3 L;
            if (walkExitVertex) L = sampleEnvAt(float4(wr.next(), wr.next(), wr.next(), wr.next()), pdfL);
            else
            {
                rng.group(2u + 3u * depth);
                L = sampleEnv(rng, pdfL);
            }
            const float3 wi = toLocal(L, t, b, n);
            float3 fs = float3(0.0), fd;
            if (walkExitVertex) fd = float3(max(dot(L, n), 0.0) * (1.0 / kPi));
            else base.evalLobes(wo, wi, tables, fs, fd);
            const float3 f = fs + fd;
            float3 vis2;
            if (pdfL > 0.0 && any(f > 0.0) && dot(L, ng) > 0.0 && visible(p, L, 1.0e4, vis2))
            {
                float bsdfPdf = fd.x;
                if (!walkExitVertex) bsdfPdf = guidePdf(gm, L, base.pdf(wo, wi, tables));
                const float3 le = envRadiance(L) * vis2 * powerHeuristic(pdfL, bsdfPdf) / pdfL;
                const float3 c = throughput * f * le;
                radiance += c;
                if (depth == 0u) { diffuseOut += throughput * fd * le; specularOut += throughput * fs * le; }
                else if (firstLobe == 1u) diffuseOut += c;
                else specularOut += c;
            }
        }

        if (walkExitVertex)
        {
            // On from the exit along the walk's cosine-weighted direction, which the next hit weighs
            // against light sampling here. The walk's iterations were not bounces: depth is the entry's
            // again (this iteration's minus one).
            if (depth == 1u) firstLobe = 1u;
            lastLobe = 1u;
            lastRough = 1.0;
            lastPdf = max(dot(dir, n), 0.0) * (1.0 / kPi);
            misOrigin = origin;
            insideVolume = false;
            --depth;
            if (depth >= 2u)
            {
                const float3 tr = throughput * max(rrRatio, 1.0);
                const float q = min(max(tr.r, max(tr.g, tr.b)), 0.95);
                if (rng.next() >= q) break;
                throughput /= q;
            }
            continue;
        }

        // The next direction: the BSDF's sample, or at a guided vertex the mixture's (a lobe with
        // probability gm.g), weighed by f cos over the mixture's exact density either way.
        BsdfSample s;
        uint guideSlot = kGuideNoSlot;
        if (guidable)
        {
            rng.group(kGuideGroup + depth);
            const float4 gu = float4(rng.next(), rng.next(), rng.next(), rng.next());
            rng.group(3u + 3u * depth);
            const float3 bu = float3(rng.next(), rng.next(), rng.next());   // the BSDF's draws (Russian roulette follows them)
            if (gu.x < gm.g)
            {
                const uint i = guidePick(gm, gu.y);
                guideSlot = gm.slot[i];
                const float3 d = vmfSample(gm.mu[i], gm.kappa[i], gu.zw);
                s.wi = toLocal(d, t, b, n);
                float3 fs, fd;
                base.evalLobes(wo, s.wi, tables, fs, fd);
                const float bsdfPdf = base.pdf(wo, s.wi, tables);
                s.pdf = guidePdf(gm, d, bsdfPdf);
                s.weight = s.pdf > 0.0 ? (fs + fd) / s.pdf : float3(0.0);
                if (bsdfPdf > 0.0) rrRatio *= s.pdf / bsdfPdf;
                s.lobe = luminance(fd) >= luminance(fs) ? 1u : 0u;
            }
            else
            {
                s = base.sample(wo, bu, tables);
                if (s.pdf > 0.0)
                {
                    const float q = guidePdf(gm, toWorld(s.wi, t, b, n), s.pdf);
                    s.weight *= s.pdf / q;
                    rrRatio *= q / s.pdf;
                    s.pdf = q;
                }
            }
            if (s.pdf > 0.0 && guideProposed < kGuideTrainVertices)
            {
                // A proposal for the grid: this vertex, the light the path had before the bounce and the
                // throughput reaching it; the ray's end is written after the next trace.
                const uint pi = pixelIndex * kGuideTrainVertices + guideProposed;
                GuideProposal gp;
                gp.x = hit.position;
                gp.normal = guideOctPack(hit.geoNormal);
                gp.xnext = float3(0.0);
                gp.flags = 0u;
                gp.rk = radiance;
                gp.meanW = gm.total / float(kGuideCandidates);
                gp.beta = throughput;
                gp.srcSlot = guideSlot;
                guideProposals[pi] = gp;
                guidePending = pi;
                ++guideProposed;
            }
        }
        else
        {
            rng.group(3u + 3u * depth);
            s = base.sample(wo, float3(rng.next(), rng.next(), rng.next()), tables);
        }
        if (!(s.pdf > 0.0)) break;
        if (s.lobe == 5u)
        {
            // Into the volume: the walk decides where the light leaves, and the path goes on from
            // there; what it meets counts in full (light sampling at the entry did not see it).
            walkMaterial = hit.material;
            wr.state = pcg(pixel.x ^ pcg(pixel.y ^ pcg(pc.params.z ^ pcg(depth ^ 0xA511E9B3u))));
            origin = hit.position - hit.geoNormal * 1e-3;
            dir = toWorld(s.wi, t, b, n);
            if (dot(dir, hit.geoNormal) >= 0.0) dir = -hit.geoNormal;
            throughput *= s.weight;
            walkWeight = float3(1.0);
            walkSteps = 0u;
            walkDist = walkDraw(wr, walkWeight, walkMedium(walkMaterial).sigmaT);
            continue;
        }
        const float3 next = toWorld(s.wi, t, b, n);
        // A refraction crosses the surface (hit.geoNormal faces the ray's side); anything else
        // below it ends the path.
        const bool crosses = transmissive && s.lobe == 4u && dot(next, hit.geoNormal) <= 0.0;
        if (!crosses && dot(next, hit.geoNormal) <= 0.0) break;
        if (depth == 0u) firstLobe = s.lobe;
        lastLobe = s.lobe;
        lastRough = s.lobe == 2u ? mi.coatRoughness : s.lobe == 3u ? 1.0 : hit.specularRoughness;
        throughput *= s.weight;
        // A ray that crossed the surface could not have been drawn by light sampling there (it
        // samples above the surface only): what it meets counts in full, not by MIS (a huge pdf). A
        // pane's crossing is null: the last vertex's light sampling saw through it, so its density
        // and place stay (the camera's ray, which has none, counts in full).
        if (crosses && mi.thinWalled && paneMaterial(hit.material))
        {
            if (lastPdf == 0.0) lastPdf = 1.0e18;
        }
        else
        {
            lastPdf = crosses ? 1.0e18 : s.pdf;
            misOrigin = p;
        }
        origin = p;
        dir = next;
        if (crosses || fromInside) ++crossings;   // a refraction, or a reflection inside a solid
        if (crosses)
        {
            origin = hit.position - hit.geoNormal * 1e-3;
            if (!mi.thinWalled)
            {
                // Into the solid, or out of it.
                insideVolume = !fromInside;
                sigmaA = float3(0.0);
                if (insideVolume && row13.x > 0.0)
                {
                    const float3 c = materials[hit.material * kMaterialRows + kTransmissionRowFirst].rgb;
                    sigmaA = -log(max(c, float3(1e-4))) / row13.x;
                }
            }
        }
        if (depth >= 2u)
        {
            const float3 tr = throughput * max(rrRatio, 1.0);
            const float q = min(max(tr.r, max(tr.g, tr.b)), 0.95);
            if (rng.next() >= q) break;
            throughput /= q;
        }
    }

    if (guiding) guidePixel[pixelIndex] = float4(radiance, asfloat(guideProposed));
    if ((flags & kFlagGuideDebug) != 0u) radiance = guideDebugColor;

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

    outputImage[pixel] = float4(displayPixel(acc, pc.tuning.z, asuint(pc.tuning.w)), 1.0);
}
#undef walkWeight
#undef walkDist
#undef walkMaterial

// ---- the radiance cache's update ------------------------------------------------------------
//
// One pixel of every 5x5 tile (a different one each frame and tile) traces a path of up to
// kCacheUpdateBounces bounces, ending in the cache beyond them, and adds to the cell of every
// vertex it records (the last kCacheUpdateVertices) the light the rest of the path brings back,
// divided by the throughput that reached the vertex: the radiance that vertex reflects, without
// its own emission. One sample a vertex. The cells' light reaches every later path that ends in
// them, so a frame of short paths carries every bounce.

static const uint kCacheUpdateBounces = 3u;
static const uint kCacheUpdateVertices = 4u;

struct CacheVertex
{
    uint slot;
    float3 throughput;
};

void cacheContribute(inout CacheVertex v[kCacheUpdateVertices], uint count, float3 c)
{
    if (!any(c > 0.0)) return;
    for (uint k = 0u; k < count; ++k)
    {
        if (v[k].slot == ~0u) continue;
        const float3 t = v[k].throughput;
        cacheAdd(cacheAccum, v[k].slot, float3(t.x > 0.0 ? c.x / t.x : 0.0, t.y > 0.0 ? c.y / t.y : 0.0,
                                                t.z > 0.0 ? c.z / t.z : 0.0));
    }
}

[shader("raygeneration")]
void cacheUpdate()
{
    const uint2 tile = DispatchRaysIndex().xy;
    const uint2 size = pc.params.xy;
    const uint o = pcg(pc.params.z * 0x9E3779B9u ^ pcg(tile.x ^ pcg(tile.y))) % 25u;
    const uint2 pixel = tile * 5u + uint2(o % 5u, o / 5u);
    if (any(pixel >= size)) return;
    Sampler rng = Sampler(pc.params.z, pcg(pixel.x ^ pcg(pixel.y ^ 0xA511E9B3u)));

    OpenPBRTables tables;
    tables.data = openpbrTables;

    const float2 uv = (float2(pixel) + float2(rng.next(), rng.next())) / float2(size);
    const float2 ndc = uv * 2.0 - 1.0;
    const float3 camPos = pc.invView[3].xyz;
    const float tanY = abs(pc.invProj[1].y);
    const float tanX = tanY * float(size.x) / float(size.y);
    float3 dir = normalize(-pc.invView[2].xyz + pc.invView[0].xyz * (ndc.x * tanX) - pc.invView[1].xyz * (ndc.y * tanY));
    float3 origin = camPos;
    float3 throughput = float3(1.0);
    float lastPdf = 0.0;

    CacheVertex verts[kCacheUpdateVertices];
    uint count = 0u;
    for (uint depth = 0u; ; ++depth)
    {
        const PathPayload hit = trace(origin, dir, depth == 0u ? kPathMask : (kPathMask | kTlasMaskEmitter), 1.0e4);
        if ((hit.flags & kHit) == 0u)
        {
            if (depth > 0u)
                cacheContribute(verts, count, throughput * envRadiance(dir) * powerHeuristic(lastPdf, envPdf(dir)));
            break;
        }
        if ((hit.flags & kHitEmitter) != 0u)
        {
            float lightPdf;
            const float3 le = emitterRadiance(hit.light, origin, hit.position, hit.geoNormal, dir, lightPdf);
            if (depth > 0u) cacheContribute(verts, count, throughput * le * powerHeuristic(lastPdf, lightPdf));
            break;
        }
        cacheContribute(verts, count, throughput * hit.emission * emissiveHitWeight(hit, origin, lastPdf, depth));
        if (depth >= kCacheUpdateBounces)
        {
            // Past the path's bounces: the light this surface reflects, as the cache holds it.
            float3 cached;
            if (cacheLookup(cacheKeys, cacheResolved, cacheCell(hit.position, hit.geoNormal, camPos), cached))
                cacheContribute(verts, count, throughput * cached);
            break;
        }

        // This vertex's cell: the light reflected here is what the rest of the path adds.
        if (count == kCacheUpdateVertices)
        {
            for (uint k = 1u; k < kCacheUpdateVertices; ++k) verts[k - 1u] = verts[k];
            count -= 1u;
        }
        verts[count].slot = cacheInsert(cacheKeys, cacheCell(hit.position, hit.geoNormal, camPos));
        verts[count].throughput = throughput;
        if (verts[count].slot != ~0u) cacheCount(cacheAccum, verts[count].slot);
        count += 1u;

        const float3 n = hit.normal;
        float3 t, b;
        frameOf(hit, t, b);
        const float3 wo = toLocal(-dir, t, b, n);
        const OpenPBRSurface base = OpenPBRSurface(inputsOf(hit));
        const float3 p = hit.position + hit.geoNormal * 1e-3;

        rng.group(1u + 3u * depth);
        const float lu0 = rng.next(), lu1 = rng.next(), lu2 = rng.next();
        const LightSample ls = sampleLight(hit.position, p, lu0, lu1, lu2);
        if (ls.valid)
        {
            const float3 wi = toLocal(ls.L, t, b, n);
            const float3 f = base.eval(wo, wi, tables);
            float3 vis5;
            if (any(f > 0.0) && dot(ls.L, hit.geoNormal) > 0.0 && visible(p, ls.L, ls.dist, vis5))
            {
                const float w = ls.pdf > 0.0 ? (ls.mis ? powerHeuristic(ls.pdf, base.pdf(wo, wi, tables)) : 1.0) / ls.pdf : 1.0;
                cacheContribute(verts, count, throughput * f * ls.radiance * vis5 * w);
            }
        }
        if (envSampled())
        {
            rng.group(2u + 3u * depth);
            float pdfL;
            const float3 L = sampleEnv(rng, pdfL);
            const float3 wi = toLocal(L, t, b, n);
            const float3 f = base.eval(wo, wi, tables);
            float3 vis6;
            if (pdfL > 0.0 && any(f > 0.0) && dot(L, hit.geoNormal) > 0.0 && visible(p, L, 1.0e4, vis6))
                cacheContribute(verts, count, throughput * f * envRadiance(L) * vis6 * powerHeuristic(pdfL, base.pdf(wo, wi, tables)) / pdfL);
        }

        rng.group(3u + 3u * depth);
        const BsdfSample s = base.sample(wo, float3(rng.next(), rng.next(), rng.next()), tables);
        if (!(s.pdf > 0.0)) break;
        const float3 next = toWorld(s.wi, t, b, n);
        if (dot(next, hit.geoNormal) <= 0.0) break;
        throughput *= s.weight;
        lastPdf = s.pdf;
        origin = p;
        dir = next;
    }
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

// The hit as closestHit reports it; anyHit fills one for a pane a shadow ray passes.
void fillHit(inout PathPayload payload, float2 bary)
{
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
    // What the raster culls (glTF's doubleSided false, material row 10's w): the side the world-space
    // winding faces away from the ray; a mirroring transform turns the winding (the normal's inverse
    // transpose does not).
    {
        const bool mirrored = determinant((float3x3)ObjectToWorld3x4()) < 0.0;
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

    // An emissive triangle is a light (render/rt/emissive_lights.hpp): its index, for the bounce's weight.
    if (any(emission > 0.0))
    {
        const uint first = emissiveFirst[InstanceID()];
        if (first != ~0u)
        {
            payload.flags |= kHitEmissive;
            payload.light = first + PrimitiveIndex();
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
        const float3 tw = mul((float3x3)ObjectToWorld3x4(), tObj.xyz);
        const float3 sn = payload.normal;
        float3 T, B;
        if (dot(tw, tw) > 1e-12 && dot(tw - sn * dot(sn, tw), tw - sn * dot(sn, tw)) > 1e-12) T = normalize(tw - sn * dot(sn, tw));
        else basisOf(sn, T, B);
        payload.tangent = rotatedTangent(sn, T, row10.z);
    }
}

[shader("closesthit")]
void closestHit(inout PathPayload payload, BuiltInTriangleIntersectionAttributes attr)
{
    fillHit(payload, attr.barycentrics);
}

[shader("anyhit")]
void anyHit(inout PathPayload payload, BuiltInTriangleIntersectionAttributes attr)
{
    const Corners c = cornersOf(InstanceID(), PrimitiveIndex());
    if ((payload.flags & kShadowRay) != 0u && paneMaterial(c.material))
    {
        // Its straight transmission at the ray's angle (the geometry's NO_DUPLICATE_ANY_HIT flag: once each).
        PathPayload h;
        h.flags = 0u;
        fillHit(h, attr.barycentrics);
        OpenPBRTables tables;
        tables.data = openpbrTables;
        const float cs = clamp(abs(dot(WorldRayDirection(), h.geoNormal)), 1e-4, 1.0);
        payload.emission *= OpenPBRSurface(inputsOf(h)).transmissionAlbedo(float3(sqrt(1.0 - cs * cs), 0.0, cs), tables);
        if (any(payload.emission > 0.0)) IgnoreHit();   // else a hit: nothing gets through
        return;
    }
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
