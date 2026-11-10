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
import rt.caustics;
import rt.manifold;
import rt.manifold_cert;
import rt.pt_hit;
import display.display;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> accumBuffer;
[[vk::binding(2, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> outputImage;
[[vk::binding(3, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> accumHistory;
[[vk::binding(6, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> albedoAOV;
[[vk::binding(7, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> normalAOV;
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
[[vk::binding(39, 0)]] RWStructuredBuffer<uint> cacheKeys;      // the radiance cache (rt/radiance_cache.slang)
[[vk::binding(40, 0)]] RWStructuredBuffer<uint4> cacheAccum;
[[vk::binding(41, 0)]] RWStructuredBuffer<uint4> cacheResolved;
// Path guiding (rt/path_guiding.slang): the states frame N-1 left, this frame's proposals and each
// pixel's sample radiance and proposal count (rt/path_guiding_update.slang reads them after).
[[vk::binding(46, 0)]] StructuredBuffer<GuideState> guideStates;
[[vk::binding(49, 0)]] RWStructuredBuffer<GuideProposal> guideProposals;
[[vk::binding(50, 0)]] RWStructuredBuffer<float4> guidePixel;
// Caustics (rt/caustics.slang, render/rt/caustic_pass.hpp): the lights' disks, the light pass's splats
// (3 words a pixel, fixed point, read and cleared by raygenCaustics), and its count of clamped splats.
[[vk::binding(51, 0)]] StructuredBuffer<float4> causticCasters;
[[vk::binding(52, 0)]] RWStructuredBuffer<uint> causticSplat;
[[vk::binding(53, 0)]] RWStructuredBuffer<uint> causticStats;
// Caustics step 2 (render/rt/manifold_casters.hpp): the owned casters' triangles, their clusters and groups, the owned
// draws and a bit word a material, and the camera side's requests (kManifoldDepths a pixel, 3 float4 each).
[[vk::binding(54, 0)]] StructuredBuffer<float4> manifoldTris;
[[vk::binding(55, 0)]] StructuredBuffer<float4> manifoldNodes;
[[vk::binding(56, 0)]] StructuredBuffer<uint4> manifoldDraws;
[[vk::binding(57, 0)]] RWStructuredBuffer<float4> manifoldRequests;

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
static const uint kFlagCaustics = 1024u;    // caustics (sub-project 24): the light pass owns sun -> smooth chain -> first hit
// OHAO_PT_CLASS (bits 11-18, caustics' raygen and the manifold pass only): keep only the light of these path classes, to find what a
// pixel's light is. A vertex is smooth when its specular roughness is under kCausticRough; "after a rough
// bounce": some earlier vertex scattered by a rough lobe.
static const uint kClassSunDirect = 1u;       // the sun's sample at a rough vertex
static const uint kClassSunGlint = 2u;        // the sun's sample at a smooth vertex after a rough bounce (step 2's)
static const uint kClassSunSpecular = 4u;     // the sun's sample at a smooth vertex, no rough bounce before
static const uint kClassEnv = 8u;             // the environment, sampled or met
static const uint kClassLights = 16u;         // every other light: points, spots, areas, emissive triangles
static const uint kClassSplat = 32u;          // the light pass's splat
static const uint kClassEnvCaustic = 64u;     // the environment through a smooth vertex after a rough bounce
static const uint kClassManifold = 128u;      // the manifold pass's connections (caustics step 2)
// Caustics step 2 (bit 19): the camera side drops the terms the manifold pass owns (sun -> the core of a smooth
// reflection at an owned material -> a rough reflection at depth 1 to kManifoldDepths); bit 20: it writes requests
// for the pass (off under OHAO_MANIFOLD_SPLIT_ONLY, which drops the terms without adding them).
static const uint kFlagManifold = 1u << 19u;
static const uint kFlagManifoldRequests = 1u << 20u;
// Bit 23: step 2b, the sun through owned solids (manifoldGlass; set with OHAO_MANIFOLD_GLASS=1, opt-in).
static const uint kFlagManifoldGlass = 1u << 23u;
static const uint kManifoldOwnedBit = 1u;
static const uint kManifoldBaseBit = 2u;
static const uint kManifoldCoatBit = 4u;
// A material's word (the draws buffer after its draws: row 1 + 2 D + w / 4).
uint manifoldMaterialBits(uint material)
{
    const uint4 h = manifoldDraws[0];
    const uint row = 1u + 3u * h.x + material / 4u;
    const uint4 r = manifoldDraws[row];
    const uint k = material & 3u;
    return h.x == 0u ? 0u : (k == 0u ? r.x : k == 1u ? r.y : k == 2u ? r.z : r.w);
}
float classKeep(uint mask, uint cls) { return mask == 0u || (mask & cls) != 0u ? 1.0 : 0.0; }
// A guided bounce's coin, lobe pick and vMF direction: Sobol group kGuideGroup + depth (outside the
// bounce groups 1 + 3d to 3 + 3d, whose sequences stay as they are).
static const uint kGuideGroup = 0x4000u;

// ---- random numbers -------------------------------------------------------------------------
// The path's draws are Owen-scrambled Sobol (sampling/sobol.slang): group 0 the camera's jitter;
// bounce d's light choice and point in group 1 + 3d, its environment sample in 2 + 3d, its BSDF
// sample and Russian roulette in 3 + 3d. PCG hashes the pixel's seed and the any-hit's coin.

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

// The camera's path through one pixel. CAUSTICS compiles in caustics' split (sub-project 24): its own
// entry and pipeline (raygenCaustics), so the default raygen carries none of its code. In the default
// one, behind a flag, it measured 8.5% of pt.trace on helmet_rt_1080p with the flag off (69.0 -> 75.0 ms).
// GUIDING compiles in path guiding (sub-project 23) the same way (raygenGuided, raygenCausticsGuided): behind its flag
// alone, off, it was 18 ms of the helmet's 61 (docs/changes/2026-11-06-pt-trace-bisect.md).
void tracePixel<let CAUSTICS : bool, let GUIDING : bool>(uint2 pixel)
{
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
    const bool guiding = GUIDING && (flags & kFlagGuiding) != 0u;
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
    // Caustics (sub-project 24, docs/specs/2026-11-03-caustics-design.md, "The split"): the light pass
    // estimates every term sun -> smooth parts -> the rough part of the camera ray's first hit, so this
    // side drops exactly those. chainThroughput is the throughput of those terms alone: at the first hit
    // the bounce's rough part over its density, then each later bounce's smooth part over its density,
    // and every other factor throughput takes (roulette, absorption, the dispersion channel). A
    // directional light's sample past the first hit subtracts chainThroughput x the smooth part there.
    const bool caustics = CAUSTICS && (flags & kFlagCaustics) != 0u;
    float3 chainThroughput = float3(0.0);
    const uint classMask = CAUSTICS ? (flags >> 11u) & 255u : 0u;   // OHAO_PT_CLASS; 0: every class
    // Caustics step 2 (docs/specs/2026-11-04-caustics-step2-design.md): chainOne is the throughput of the terms the
    // manifold pass owns since the last bounce, a rough reflection at depth 1 to kManifoldDepths (every factor
    // throughput takes until the next bounce resets it); the next vertex's directional light sample drops chainOne x
    // the core of its smooth reflection when its material is owned.
    const bool manifold = caustics && (flags & kFlagManifold) != 0u;
    float3 chainOne = float3(0.0);
    if (CAUSTICS && (flags & kFlagManifoldRequests) != 0u)
        for (uint k = 0u; k < kManifoldDepths; ++k) manifoldRequests[(pixel.y * size.x + pixel.x) * kManifoldDepths * 3u + k * 3u] = float4(0.0);
    bool roughBefore = false;                                        // an earlier vertex scattered by a rough lobe
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
            if (insideVolume)
            {
                const float3 absorbed = exp(-sigmaA * ((hit.flags & kHit) != 0u ? hit.t : 1.0e4));
                throughput *= absorbed;
                chainThroughput *= absorbed;
                if (CAUSTICS) chainOne *= absorbed;
            }
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
            if (CAUSTICS)
            {
                const uint addClass = (hit.flags & kHit) != 0u ? kClassLights
                                      : depth > 0u && lastRough < kCausticRough && roughBefore ? kClassEnvCaustic : kClassEnv;
                radiance += add * classKeep(classMask, addClass);
            }
            else radiance += add;
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
                const float3 pick = float3(channel == 0 ? 3.0 : 0.0, channel == 1 ? 3.0 : 0.0, channel == 2 ? 3.0 : 0.0);
                throughput *= pick;
                chainThroughput *= pick;
                if (CAUSTICS) chainOne *= pick;
            }
            mi.specularIor = dispersedIor(mi.specularIor, row13.w, row13.z, kDispersionLambda[channel]);
        }
        const OpenPBRSurface base = OpenPBRSurface(mi);
        // The surface seen from wo, once for every direction below (a thin wall seen from its back as its front,
        // mirrored, as OpenPBR's sample() and pdf() see it). Each OpenPBR call site is inlined whole (Slang, for
        // SPIR-V): this vertex weighs its light's, the environment's and the bounce's directions at one site each,
        // in one loop (sub-project 25: raygen was 1.1 million instructions, an 11-minute cold compile).
        const bool back = base.thinBack(wo);
        const float3 o = back ? base.mirrored(wo) : wo;
        const OpenPBRView surfView = base.view(o, tables);
        const float4 shares = base.drawShares(o, surfView, tables);
        if (depth == 0u)
        {
            firstRough = clamp(hit.specularRoughness, 0.01, 1.0);
            roughnessAOV[pixel] = firstRough;
            const float3 a = base.albedoFrom(wo, surfView, tables);
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

        // ---- the vertex's directions ----
        // Next-event estimation's (a light, then the environment, each weighed against the BSDF) and the bounce's,
        // all drawn before any is weighed. The draws are the groups they always were: 1 + 3 depth the light, 2 + 3
        // depth the environment, kGuideGroup + depth the guide, 3 + 3 depth the bounce; nothing draws while they are
        // weighed, so Russian roulette after reads the bounce group's fourth coordinate, as a walk's exit does.
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
        const bool envOn = envSampled();
        float pdfL = 0.0;
        float3 L = float3(0.0, 0.0, 1.0);
        if (envOn)
        {
            if (walkExitVertex) L = sampleEnvAt(float4(wr.next(), wr.next(), wr.next(), wr.next()), pdfL);
            else
            {
                rng.group(2u + 3u * depth);
                L = sampleEnv(rng, pdfL);
            }
        }

        // Caustics step 2: a vertex at depth 1 to kManifoldDepths whose next vertex's light sample the bounce limit
        // allows (a reflection from outside a solid, not inside an absorbing one; not a walk's exit) is a manifold pass's X: its request is the
        // ray that met it and the throughput before its bounce, whatever the bounce then draws.
        bool manifoldX = false;
        // The next direction: the BSDF's draw, or at a guided vertex the mixture's (a lobe with probability gm.g),
        // weighed below by f cos over the mixture's exact density either way.
        BsdfSample s = bsdfSampleNone();
        bool bounce = false;      // s's direction is weighed below (else s is final: none, or the walk's)
        bool baseDrawn = false;   // a layered glass's base drew it
        bool guided = false;
        float3 guideDir = float3(0.0);
        uint guideSlot = kGuideNoSlot;
        if (!walkExitVertex)
        {
            if (manifold)
            {
                manifoldX = depth >= 1u && depth <= kManifoldDepths && !fromInside && !insideVolume &&
                            depth + 1u - crossings <= bounces && crossings <= kCrossings;
                if (manifoldX && (flags & kFlagManifoldRequests) != 0u)
                {
                    const uint r = (pixelIndex * kManifoldDepths + depth - 1u) * 3u;
                    manifoldRequests[r] = float4(origin, asfloat(1u | uint(channel + 1) << 1u));
                    manifoldRequests[r + 1u] = float4(dir, 0.0);
                    manifoldRequests[r + 2u] = float4(throughput, 0.0);
                }
            }
            float3 bu;
            if (guidable)
            {
                rng.group(kGuideGroup + depth);
                const float4 gu = float4(rng.next(), rng.next(), rng.next(), rng.next());
                rng.group(3u + 3u * depth);
                bu = float3(rng.next(), rng.next(), rng.next());   // the BSDF's draws (Russian roulette follows them)
                guided = gu.x < gm.g;
                if (guided)
                {
                    const uint i = guidePick(gm, gu.y);
                    guideSlot = gm.slot[i];
                    guideDir = vmfSample(gm.mu[i], gm.kappa[i], gu.zw);
                    s.wi = toLocal(guideDir, t, b, n);
                    bounce = true;
                }
            }
            else
            {
                rng.group(3u + 3u * depth);
                bu = float3(rng.next(), rng.next(), rng.next());
            }
            if (!guided) bounce = base.sampleDirectionFrom(o, bu, surfView, shares, tables, s, baseDrawn);
            if (back) s.wi = base.mirrored(s.wi);
        }

        // ---- each direction weighed ----
        // k = 0 the light's, 1 the environment's, 2 the bounce's, every term before any shadow ray: a sample's light is
        // kept without its visibility (every term of it is linear in it) and the vertex's one shadow ray multiplies it in
        // below, so neither the surface nor its view is live across a trace (they were: 7 ms of the helmet's 61).
        float3 sB = float3(0.0), cB = float3(0.0), dB = float3(0.0);   // the bounce's terms (its caustic split)
        // The light's (0) and the environment's (1): the light before visibility, its diffuse and specular parts (NRD's).
        float3 nee0 = float3(0.0), neeD0 = float3(0.0), neeS0 = float3(0.0);
        float3 nee1 = float3(0.0), neeD1 = float3(0.0), neeS1 = float3(0.0);
        bool shadow0 = false, shadow1 = false;
        [loop]
        for (uint k = 0u; k < 3u; ++k)
        {
            if (k == 0u ? !ls.valid : k == 1u ? !envOn : !bounce) continue;
            const float3 Lw = k == 0u ? ls.L : L;   // a light sample's direction, in the world
            const float3 wi = k == 2u ? s.wi : toLocal(Lw, t, b, n);
            const float3 iFront = back ? base.mirrored(wi) : wi;
            float3 fs = float3(0.0), fd, sT = float3(0.0), cT = float3(0.0);
            if (walkExitVertex) fd = float3(max(dot(Lw, n), 0.0) * (1.0 / kPi));
            else
            {
                // The bounce and the caustic light sample are weighed in the front frame (as OpenPBR's sample() and
                // evalParts() take them); light sampling's split is evalLobes()' (the camera side meets a thin wall
                // from its front, where both are one).
                const bool front = k == 2u || (CAUSTICS && k == 0u);
                base.terms(front ? o : wo, front ? iFront : wi, surfView, tables, sT, cT, fd);
                fs = sT + cT;
            }
            const float3 f = fs + fd;
            if (k < 2u && !((k == 0u || pdfL > 0.0) && any(f > 0.0) && dot(Lw, ng) > 0.0)) continue;
            float bsdfPdf = fd.x, basePdf = 0.0;
            if (!walkExitVertex && (k != 0u || (ls.pdf > 0.0 && ls.mis)))
                bsdfPdf = base.pdfFrom(o, iFront, surfView, shares, tables, basePdf);
            if (k == 2u)
            {
                sB = sT;
                cB = cT;
                dB = fd;
                if (guided)
                {
                    s.pdf = guidePdf(gm, guideDir, bsdfPdf);
                    s.weight = s.pdf > 0.0 ? f / s.pdf : float3(0.0);
                    if (bsdfPdf > 0.0) rrRatio *= s.pdf / bsdfPdf;
                    s.lobe = luminance(fd) >= luminance(fs) ? 1u : 0u;
                }
                else
                {
                    base.sampleWeigh(s, bsdfPdf, basePdf, baseDrawn, f);
                    if (guidable && s.pdf > 0.0)
                    {
                        const float q = guidePdf(gm, toWorld(s.wi, t, b, n), s.pdf);
                        s.weight *= s.pdf / q;
                        rrRatio *= q / s.pdf;
                        s.pdf = q;
                    }
                }
                continue;
            }
            float3 le;   // before visibility
            if (k == 0u)
            {
                // A sun is a delta (its radiance already over its choice); a light without an
                // emitter mesh only light sampling finds: full weight.
                float w = 1.0;
                if (ls.pdf > 0.0)
                {
                    if (ls.mis && !walkExitVertex) bsdfPdf = guidePdf(gm, ls.L, bsdfPdf);
                    w = (ls.mis ? powerHeuristic(ls.pdf, bsdfPdf) : 1.0) / ls.pdf;
                }
                le = ls.radiance * w;
            }
            else
            {
                if (!walkExitVertex) bsdfPdf = guidePdf(gm, L, bsdfPdf);
                le = envRadiance(L) * powerHeuristic(pdfL, bsdfPdf) / pdfL;
            }
            float3 c = throughput * f * le;
            if (CAUSTICS && k == 0u && !walkExitVertex)
            {
                float3 smoothNee, coreNee, rough;   // the light sample's smooth part and its core
                base.split(sT, cT, fd, kCausticRough, smoothNee, rough);
                coreNee = back ? float3(0.0) : base.coreOf(o, iFront, sT, cT, kCausticRough,
                                                           float2(kManifoldCoreSlope2, kManifoldCoreMinSlope2));
                if (caustics && depth > 0u && ls.pdf == 0.0 && any(chainThroughput > 0.0))
                {
                    // A directional light through an all-smooth chain from the first hit: the light pass's.
                    c -= chainThroughput * smoothNee * le;
                }
                if (manifold && ls.pdf == 0.0 && any(chainOne > 0.0) && (manifoldMaterialBits(hit.material) & kManifoldOwnedBit) != 0u)
                {
                    // A directional light through the core of a smooth reflection after a rough one: the manifold pass's.
                    c -= chainOne * coreNee * le;
                }
            }
            float3 kept = c;
            if (CAUSTICS)
            {
                const bool smoothVertex = !walkExitVertex && hit.specularRoughness < kCausticRough;
                const uint sunClass = !smoothVertex ? kClassSunDirect : roughBefore ? kClassSunGlint : kClassSunSpecular;
                const uint cls = k == 0u ? (ls.pdf == 0.0 ? sunClass : kClassLights)
                                         : smoothVertex && roughBefore ? kClassEnvCaustic : kClassEnv;
                kept = c * classKeep(classMask, cls);
            }
            // The first hit's direct light by lobe (NRD's split); later ones by the first lobe.
            float3 dPart = float3(0.0), sPart = float3(0.0);
            if (depth == 0u) { dPart = throughput * fd * le; sPart = throughput * fs * le; }
            else if (firstLobe == 1u) dPart = c;
            else sPart = c;
            if (k == 0u) { nee0 = kept; neeD0 = dPart; neeS0 = sPart; shadow0 = true; }
            else { nee1 = kept; neeD1 = dPart; neeS1 = sPart; shadow1 = true; }
        }
        // The light's and the environment's shadow ray, the vertex's one (panes multiply their transmittance in).
        [loop]
        for (uint k = 0u; k < 2u; ++k)
        {
            if (!(k == 0u ? shadow0 : shadow1)) continue;
            float3 vis = float3(1.0);
            if (!visible(p, k == 0u ? ls.L : L, k == 0u ? ls.dist : 1.0e4, vis)) continue;
            radiance += (k == 0u ? nee0 : nee1) * vis;
            diffuseOut += (k == 0u ? neeD0 : neeD1) * vis;
            specularOut += (k == 0u ? neeS0 : neeS1) * vis;
        }

        if (walkExitVertex)
        {
            // On from the exit along the walk's cosine-weighted direction, which the next hit weighs
            // against light sampling here. The walk's iterations were not bounces: depth is the entry's
            // again (this iteration's minus one).
            if (depth == 1u) firstLobe = 1u;
            lastLobe = 1u;
            lastRough = 1.0;
            if (CAUSTICS) roughBefore = true;
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
                chainThroughput /= q;
                if (CAUSTICS) chainOne /= q;
            }
            continue;
        }

        if (guidable && s.pdf > 0.0 && guideProposed < kGuideTrainVertices)
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
        if (!(s.pdf > 0.0)) break;
        if (s.lobe == 5u)
        {
            // Into the volume: the walk decides where the light leaves, and the path goes on from
            // there; what it meets counts in full (light sampling at the entry did not see it).
            chainThroughput = float3(0.0);
            if (CAUSTICS) chainOne = float3(0.0);
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
        if (CAUSTICS && (s.lobe == 1u || s.lobe == 3u || s.lobe == 5u || lastRough >= kCausticRough)) roughBefore = true;
        if (caustics)
        {
            float3 sm, ro;
            base.split(sB, cB, dB, kCausticRough, sm, ro);
            chainThroughput = depth == 0u ? throughput * ro / s.pdf : chainThroughput * sm / s.pdf;
            // Caustics step 2: a rough reflection at depth 1 to kManifoldDepths starts a chain of one (its request was
            // written before the draw); any other bounce ends it.
            if (CAUSTICS) chainOne = manifold && manifoldX && !crosses ? throughput * ro / s.pdf : float3(0.0);
        }
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
            chainThroughput /= q;
            if (CAUSTICS) chainOne /= q;
        }
    }

    if (caustics)
    {
        // What the light pass splatted on this pixel this frame (the terms the split left to it).
        const uint c = pixelIndex * 3u;
        const float3 light = float3(float(causticSplat[c]), float(causticSplat[c + 1u]), float(causticSplat[c + 2u])) *
                             (1.0 / kCausticSplatScale);
        causticSplat[c] = 0u;
        causticSplat[c + 1u] = 0u;
        causticSplat[c + 2u] = 0u;
        radiance += light * classKeep(classMask, kClassSplat);
        diffuseOut += light;
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

[shader("raygeneration")]
void raygen()
{
    tracePixel<false, false>(DispatchRaysIndex().xy);
}

[shader("raygeneration")]
void raygenCaustics()
{
    tracePixel<true, false>(DispatchRaysIndex().xy);
}

[shader("raygeneration")]
void raygenGuided()
{
    tracePixel<false, true>(DispatchRaysIndex().xy);
}

[shader("raygeneration")]
void raygenCausticsGuided()
{
    tracePixel<true, true>(DispatchRaysIndex().xy);
}

// ---- caustics step 2: manifold connections (docs/specs/2026-11-04-caustics-step2-design.md) ----------------------

// A hit's surface as tracePixel builds it (its inputs, the dispersion's IOR at `channel`, its frame), with its shading
// normal replaced by `normal` when that is not zero (the manifold's solved normal at S).
struct ManifoldSurface
{
    MaterialInputs mi;
    float3 n, t, b;
    bool fromInside;
};

ManifoldSurface manifoldSurface(PathPayload hit, int channel, float3 normal)
{
    ManifoldSurface s;
    s.mi = inputsOf(hit);
    const bool transmissive = s.mi.transmissionWeight > 0.0 && s.mi.baseMetalness < 1.0;
    s.fromInside = transmissive && !s.mi.thinWalled && (hit.flags & kHitBack) != 0u;
    const float3 hn = dot(normal, normal) > 0.0 ? normal : hit.normal;
    s.n = s.fromInside ? -hn : hn;
    if (dot(hit.tangent, hit.tangent) > 0.0)
    {
        const float3 tt = hit.tangent - s.n * dot(s.n, hit.tangent);
        if (!(dot(tt, tt) > 1e-12)) basisOf(s.n, s.t, s.b);
        else
        {
            s.t = normalize(tt);
            s.b = cross(s.n, s.t);
        }
    }
    else
    {
        basisOf(s.n, s.t, s.b);
    }
    const float4 row13 = materials[hit.material * kMaterialRows + kTransmissionRowFirst + 1u];
    // max(channel, 0): a call with channel -1 inlines the index as a constant, which SPIR-V refuses even unreached.
    if (transmissive && row13.z > 0.0 && channel >= 0)
        s.mi.specularIor = dispersedIor(s.mi.specularIor, row13.w, row13.z, kDispersionLambda[max(channel, 0)]);
    return s;
}

// A draw's weight for its choice from p: its world bounding sphere's solid angle (over 2 pi), 0 when no lobe of it can
// be smooth (the draws buffer's third row: centre, radius; its alpha bounds in the gather's record).
float manifoldDrawWeight(uint d, uint4 header, float3 p)
{
    const float2 alphas = manifoldNodes[(header.w + header.z + d) * 4u].xy;
    if (!(alphas.x > 0.0 || alphas.y > 0.0)) return 0.0;
    const float4 s = asfloat(manifoldDraws[3u + 3u * d]);
    const float3 v = s.xyz - p;
    const float d2 = dot(v, v), r2 = s.w * s.w;
    if (!(d2 > r2)) return 1.0;
    return 1.0 - sqrt(1.0 - r2 / d2);
}

uint manifoldHash(uint a, uint b, uint c, uint d) { return pcg(a ^ pcg(b ^ pcg(c ^ pcg(d ^ 0x3C6EF372u)))); }
float manifoldUnit(uint h) { return float(h >> 8u) * (1.0 / 16777216.0); }

// ---- step 2b: the sun through a solid (docs/specs/2026-11-04-caustics-step2-design.md, "Step 2b as designed") ----

// The walk's scene (rt/manifold_cert.slang's IManifoldScene): one instance's triangles in world space. Its rays are forced
// non-opaque, so every candidate comes to the loop and only the instance's own are committed; every triangle takes the
// request's frame reference c (a pair's slopes are in frames that must not change between the walks of one request).
// Each ray is clipped to the instance's bounding sphere (grown 2%), which holds every triangle of it: outside it the
// traversal would hand back every other instance's triangles along the ray for nothing.
struct ManifoldInstanceScene : IManifoldScene
{
    uint slot;
    float3 c;
    float4 sphere;   // centre, radius: the mesh's world bounds

    bool trace(float3 o, float3 d, float tMax, out uint tri, out float2 uv, out float t)
    {
        tri = 0u;
        uv = float2(0.0);
        t = 0.0;
        const float3 oc = o - sphere.xyz;
        const float dd = dot(d, d);
        const float bh = dot(oc, d);
        const float rad = 1.02 * sphere.w + 1e-4;
        const float disc = bh * bh - dd * (dot(oc, oc) - rad * rad);
        if (!(disc >= 0.0) || !(dd > 0.0)) return false;
        const float sq = sqrt(disc);
        const float t1 = (-bh + sq) / dd;
        if (!(t1 > 1e-6)) return false;
        RayDesc ray;
        ray.Origin = o;
        ray.Direction = d;
        ray.TMin = max((-bh - sq) / dd, 1e-6);
        ray.TMax = min(min(tMax, 1.0e4), t1);
        RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
        q.TraceRayInline(scene, RAY_FLAG_FORCE_NON_OPAQUE, kPathMask, ray);
        while (q.Proceed())
        {
            if (q.CandidateInstanceID() == slot) q.CommitNonOpaqueTriangleHit();
        }
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
        tri = q.CommittedPrimitiveIndex();
        uv = q.CommittedTriangleBarycentrics();
        t = q.CommittedRayT();
        return true;
    }

    ManifoldTri triangle(uint i)
    {
        const uint stride = kInstanceGpuStride / 16u;
        const uint4 mesh = meshes[asuint(instances[slot * stride + 8u]).x];
        const uint base = mesh.y + i * 3u;
        const uint v0 = mesh.x + indices[base], v1 = mesh.x + indices[base + 1u], v2 = mesh.x + indices[base + 2u];
        ManifoldTri tri = manifoldTriangleFrom(instances[slot * stride], instances[slot * stride + 1u],
                                               instances[slot * stride + 2u], instances[slot * stride + 3u],
                                               vertexF3(v0, 0u), vertexF3(v1, 0u), vertexF3(v2, 0u), vertexF3(v0, 6u),
                                               vertexF3(v1, 6u), vertexF3(v2, 6u));
        tri.c = c;
        return tri;
    }
}

// A path the walk found (y1 on A at first.x.xy, y2 on B at first.x.zw), checked as the camera side would trace it, and
// its light before the re-find count: X -> y2 and y2 -> y1 must meet the solid first at the walk's distances (its
// material; from outside at y2, from inside at y1), the sun must reach y1's outer side, and the three vertices' BSDFs in
// the camera's roles (OpenPBR is not reciprocal), at one evaluation site (each OpenPBR call site is compiled whole):
// X's rough part, wo toward the camera side and wi toward y2; the refraction at y2, wo toward X and wi toward y1; at
// y1, wo toward y2 (below its outward frame) and wi toward the sun (smooth + rough: the whole lobe). 0 if refused.
float3 manifoldGlassPath(ManifoldTri A, ManifoldTri B, ManifoldWalkResult first, uint material, float alpha, float4 target,
                         float3 pX, float3 gX, float3 woX, ManifoldSurface sx, OpenPBRTables tables, float3 L, float3 E,
                         int ch, float3 chMask)
{
    const float4 row12 = materials[material * kMaterialRows + kTransmissionRowFirst];
    const float4 row13 = materials[material * kMaterialRows + kTransmissionRowFirst + 1u];
    const float3 y1 = manifoldPoint(A, first.x.xy), y2 = manifoldPoint(B, first.x.zw);
    const float3 toY2 = y2 - pX;
    const float rX = length(toY2);
    const float3 wX = toY2 / rX;   // X -> y2
    if (dot(wX, gX) <= 0.0) return float3(0.0);
    const float3 inner = y1 - y2;
    const float rIn = length(inner);
    const float3 back = inner / rIn;   // y2 -> y1
    PathPayload hs[2];
    for (uint k = 0u; k < 2u; ++k)
    {
        const float3 o = k == 0u ? pX : y2 + back * (1e-4 * rIn);
        const float3 dir = k == 0u ? wX : back;
        const float expect = k == 0u ? rX : rIn * (1.0 - 1e-4);
        const PathPayload p = trace(o, dir, kPathMask | kTlasMaskEmitter, 1.0e4, 0.0);
        if ((p.flags & kHit) == 0u || (p.flags & kHitEmitter) != 0u || p.material != material ||
            abs(p.t - expect) > 1e-4 * expect + 1e-5)
            return float3(0.0);
        hs[k] = p;
    }
    // The sun at y1, from outside it, off its outer side.
    const float3 out1 = -hs[1].geoNormal;   // the hit's geometric normal faces the ray (inside)
    if (dot(L, out1) <= 0.0) return float3(0.0);
    float3 T;
    if (!visible(y1 + out1 * 1e-3, L, 1.0e4, T)) return float3(0.0);
    float3 f = float3(1.0);
    for (uint k = 0u; k < 3u; ++k)
    {
        ManifoldSurface s = sx;
        if (k > 0u)
        {
            s = manifoldSurface(hs[k - 1u], ch, float3(0.0));
            if (s.fromInside != (k == 2u)) return float3(0.0);   // a solid: met from outside at y2, from inside at y1
        }
        const float3 a = k == 0u ? woX : k == 1u ? -wX : -back;
        const float3 b = k == 0u ? wX : k == 1u ? back : L;
        float3 sm, ro;
        OpenPBRSurface(s.mi).evalParts(toLocal(a, s.t, s.b, s.n), toLocal(b, s.t, s.b, s.n), tables, kCausticRough, sm, ro);
        f *= k == 0u ? ro : sm + ro;
    }
    if (!any(f > 0.0)) return float3(0.0);
    // The solid's inside absorbs over y1 -> y2 (transmission_color at transmission_depth, as the camera side's volume).
    float3 absorb = float3(1.0);
    if (row13.x > 0.0) absorb = exp(log(max(row12.rgb, float3(1e-4))) * (rIn / row13.x));
    // Area to slopes: |det ds / dA| = slopeDet / (|e1 x e2| at y1 x at y2); each area to its solid angle at the vertex
    // before it by its geometric cosine over the distance squared.
    const float3 g1 = cross(A.e1, A.e2), g2 = cross(B.e1, B.e2);
    const float pdf = manifoldGgxSlopeDensity(alpha, target.xy) * manifoldGgxSlopeDensity(alpha, target.zw);
    const float k = abs(dot(wX, g2)) * abs(dot(back, g1)) / (rX * rX * rIn * rIn * first.slopeDet * pdf);
    const float3 c = f * E * T * absorb * chMask * k;
    return all(isfinite(c)) ? c : float3(0.0);
}

// Re-find trials a found path is counted by, at most (the first walk among them): a ceiling, not a cap. Zeltner et
// al.'s count is exact only uncapped, and a path near a fold is found again rarely and weighs much: at 64 the ball
// focused on the floor read 30% dark (check_caustics m10_ball_focus); no path there needs 1,024. Uncapped the walks
// a sample costs are on average one a reachable root (a path found with probability p takes 1/p walks); the ceiling
// only bounds a warp's worst lane, and paths that reach it are counted (the stats' word 11, the teardown log).
static const uint kManifoldGlassTrials = 4096u;

// Step 2b's light at X from one directional light (direction L, illuminance E) through one solid instance: sun -> into
// the solid at y1 -> out at y2 -> X's rough part, each refraction over its whole lobe (the camera side renders none of
// it: its light samples look above a surface only and its shadow rays stop at solids, ruling M12). Without beta and the
// draw's choice. The slopes s1, s2 are drawn from GGX's at the draw's alpha; the seeded walk (rt/manifold_cert.slang,
// uncertified) finds a path from a seed drawn in the cone the solid's bounding sphere subtends from X, counted by the
// walks it takes to find the same path again (Zeltner et al. 2020: exact in expectation for every path a seed reaches;
// up to kManifoldGlassTrials). The estimate and its walk are manifold_tests t15's GPU form, held there to light
// traced through a rough glass ball. The first walk's path is checked and weighed (manifoldGlassPath) before any
// re-find walk: a refused path costs none. The walk has one call site (each is compiled whole). counts: (walks, paths
// found, connections, paths at the trials' ceiling).
float3 manifoldGlass(uint slot, uint material, float4 sphere, float alpha, float3 pX, float3 gX, float3 woX,
                     ManifoldSurface sx, OpenPBRTables tables, float3 L, float3 E, int channel, uint h, inout uint4 counts)
{
    const float4 row13 = materials[material * kMaterialRows + kTransmissionRowFirst + 1u];
    // A dispersive solid draws the path's wavelength unless the path has one, as the camera side does there: that
    // channel alone, three times over.
    int ch = channel;
    float3 chMask = float3(1.0);
    if (ch < 0 && row13.z > 0.0)
    {
        ch = int(min(manifoldUnit(h) * 3.0, 2.0));
        chMask = float3(ch == 0 ? 3.0 : 0.0, ch == 1 ? 3.0 : 0.0, ch == 2 ? 3.0 : 0.0);
    }
    // The walk refracts at the IOR the BSDF refracts at: the specular IOR (dispersed for the channel) modulated by
    // specular_weight and clamped, as OpenPBRBase. A coat makes the base's IOR the coat-relative one and adds an
    // interface the two refractions do not model, and a specular-weight texture gives each texel its own IOR: neither
    // is step 2b's (the caller's gate).
    const float ior = materials[material * kMaterialRows + 5u].x;
    const float etaSpec = ch >= 0 && row13.z > 0.0 ? dispersedIor(ior, row13.w, row13.z, kDispersionLambda[max(ch, 0)]) : ior;
    const float eta = clamp(modulatedIor(etaSpec, materials[material * kMaterialRows + 4u].w), 1.0, kPbrEtaMax);
    uint r = pcg(h ^ 0x2B5EED5u);
    const uint r0 = pcg(r), r1 = pcg(r0), r2 = pcg(r1), r3 = pcg(r2);
    r = r3;
    const float4 target = float4(manifoldGgxSlope(alpha, float2(manifoldUnit(r0), manifoldUnit(r1))),
                                 manifoldGgxSlope(alpha, float2(manifoldUnit(r2), manifoldUnit(r3))));
    // The slopes' frame, the same at every walk of the request: perpendicular to the sun and to the solid's direction
    // from X, so a normal along it (the frame's pole) is one the sun grazes or one X sees edge-on.
    const float3 toC = sphere.xyz - pX;
    const float dC = length(toC);
    if (!(dC > 0.0)) return float3(0.0);
    float3 c = cross(L, toC);
    if (dot(c, c) > 1e-8 * dC * dC) c = normalize(c);
    else
    {
        float3 cb;
        basisOf(L, c, cb);
    }
    ManifoldInstanceScene sc;
    sc.slot = slot;
    sc.c = c;
    sc.sphere = sphere;
    // Seeds uniform in the cone the solid's bounding sphere (grown 2%) subtends from X; every direction from inside it.
    const float3 axis = toC / dC;
    const float sinA = min(1.0, 1.02 * sphere.w / dC);
    const float cosA = dC > 1.02 * sphere.w ? sqrt(1.0 - sinA * sinA) : -1.0;
    float3 ta, tb;
    basisOf(axis, ta, tb);
    ManifoldWalkResult first;
    first.found = false;
    first.entry = 0u;
    first.exit = 0u;
    first.x = float4(0.0);
    first.slopeDet = 0.0;
    float3 light = float3(0.0);
    uint trials = 0u;
    for (uint k = 0u; k < kManifoldGlassTrials; ++k)
    {
        r = pcg(r);
        const float uc = manifoldUnit(r);
        r = pcg(r);
        const float cz = cosA + (1.0 - cosA) * uc, sz = sqrt(max(0.0, 1.0 - cz * cz));
        const float ph = 2.0 * kManifoldPi * manifoldUnit(r);
        const float3 seedDir = axis * cz + ta * (sz * cos(ph)) + tb * (sz * sin(ph));
        const ManifoldWalkResult w = manifoldWalkPair<ManifoldInstanceScene, false>(sc, pX, L, eta, target, seedDir);
        ++counts.x;
        if (k == 0u)
        {
            if (!w.found || !(w.slopeDet > 0.0)) return float3(0.0);
            first = w;
            ++counts.y;
            light = manifoldGlassPath(sc.triangle(w.entry), sc.triangle(w.exit), w, material, alpha, target, pX, gX, woX,
                                      sx, tables, L, E, ch, chMask);
            if (!any(light > 0.0)) return float3(0.0);
            trials = 1u;
            continue;
        }
        // The same path: the same pair and root (the walk polishes it in the pair's own parameters).
        if (w.found && w.entry == first.entry && w.exit == first.exit && all(abs(w.x - first.x) < 1e-3)) break;
        ++trials;
    }
    ++counts.z;
    if (trials == kManifoldGlassTrials) ++counts.w;
    return light * float(trials);
}

// The manifold pass (render/rt/path_tracer_render.cpp, after raygenCaustics): a thread a pixel over its requests. Each
// retraces the ray that met X, and for each directional light and owned draw draws one slope in the draw's smooth
// lobes' cores, solves every triangle the cones keep for the points whose half vector between X and the light has that
// slope, and connects them (X -> y and y -> the light visible): the term sun -> the core at y -> X's rough reflection,
// by the change of variables from the slope to y's area. The sum goes into the running mean the raygen just wrote.
[shader("raygeneration")]
void manifoldConnect()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 size = pc.params.xy;
    const uint pixelIndex = pixel.y * size.x + pixel.x;
    const uint flags = pc.control.x;
    const uint classMask = (flags >> 11u) & 255u;
    const uint debugStage = (flags >> 21u) & 3u;   // OHAO_MANIFOLD_STAGE: 3 stop after X, 1 at the candidates, 2 at the roots
    OpenPBRTables tables;
    tables.data = openpbrTables;
    const uint4 header = manifoldDraws[0];   // (D, T, C, G)
    const uint lights = asuint(causticCasters[0].x);
    float3 sum = float3(0.0);
    uint requests = 0u, candidates = 0u, roots = 0u, connections = 0u;
    uint clusterTests = 0u, subTests = 0u, triTests = 0u;
    uint4 glassCounts = uint4(0u);   // step 2b: walks, paths found, connections, at the trials' ceiling
    for (uint slot = 0u; slot < kManifoldDepths; ++slot)
    {
        const uint r = (pixelIndex * kManifoldDepths + slot) * 3u;
        const float4 r0 = manifoldRequests[r];
        const uint meta = asuint(r0.w);
        if ((meta & 1u) == 0u) continue;
        ++requests;
        const float3 rayDir = manifoldRequests[r + 1u].xyz;
        const float3 beta = manifoldRequests[r + 2u].xyz;
        const int channel = int((meta >> 1u) & 3u) - 1;
        const PathPayload hx = trace(r0.xyz, rayDir, kPathMask | kTlasMaskEmitter, 1.0e4);
        if ((hx.flags & kHit) == 0u || (hx.flags & kHitEmitter) != 0u) continue;
        const ManifoldSurface sx = manifoldSurface(hx, channel, float3(0.0));
        if (sx.fromInside) continue;
        const OpenPBRSurface bx = OpenPBRSurface(sx.mi);
        const float3 wo = toLocal(-rayDir, sx.t, sx.b, sx.n);
        const float3 pX = hx.position + hx.geoNormal * 1e-3;
        if (debugStage == 3u) continue;
        for (uint l = 0u; l < lights; ++l)
        {
            const float4 la = causticCasters[2u + 3u * l], lb = causticCasters[3u + 3u * l];
            const float3 L = -normalize(la.xyz);
            const float3 E = lb.rgb;
            // One owned draw a request and light, chosen by its solid angle from X: the estimate divides by its
            // probability (exact while every draw that can hold a root has one above 0), and a convex caster's glint
            // at X scales with its solid angle. Six draws walked a request cost the showcase 300 ms a frame.
            float wSum = 0.0;
            for (uint e = 0u; e < header.x; ++e) wSum += manifoldDrawWeight(e, header, pX);
            if (!(wSum > 0.0)) continue;
            // Russian roulette on the request by the casters' solid angle from X (their glints scale with it): one far
            // from every caster runs with probability wSum / kManifoldRouletteWeight, weighed by its inverse (exact).
            const float keep = min(wSum / kManifoldRouletteWeight, 1.0);
            if (keep < 1.0 && manifoldUnit(manifoldHash(pixelIndex, pc.params.z, slot * 64u + l, 0x0B5EEDu)) >= keep) continue;
            const float uPick = manifoldUnit(manifoldHash(pixelIndex, pc.params.z, slot * 64u + l, 0x5EEDu)) * wSum;
            uint d = 0u;
            float wPick = 0.0, acc = 0.0;
            for (uint e = 0u; e < header.x; ++e)
            {
                const float w = manifoldDrawWeight(e, header, pX);
                acc += w;
                if (w > 0.0)
                {
                    d = e;
                    wPick = w;
                    if (uPick < acc) break;
                }
            }
            const float invP = wSum / (wPick * keep);
            {
                const uint4 da = manifoldDraws[1u + 3u * d], db = manifoldDraws[2u + 3u * d];
                // The draw's lobes' alpha bounds, from its material's factors (the gather's record).
                const float2 alphas = manifoldNodes[(header.w + header.z + d) * 4u].xy;
                if (!(alphas.x > 0.0 || alphas.y > 0.0)) continue;
                const uint h0 = manifoldHash(pixelIndex, pc.params.z, slot * 64u + l, d);
                const uint h1 = pcg(h0), h2 = pcg(h1);
                const float2 coreSlope = float2(kManifoldCoreSlope2, kManifoldCoreMinSlope2);
                const float2 target = manifoldDrawSlope(alphas, coreSlope,
                                                        float3(manifoldUnit(h0), manifoldUnit(h1), manifoldUnit(h2)));
                const float thetaS = atan(length(target));
                const float aMin = alphas.x > 0.0 && alphas.y > 0.0 ? min(alphas.x, alphas.y) : max(alphas.x, alphas.y);
                const float tol = max(1e-3 * aMin, 1e-6);
                const uint clustersInDraw = da.y;   // a cluster a meshlet (rt/manifold_gather.slang)
                const uint groupsInDraw = (clustersInDraw + kManifoldCluster - 1u) / kManifoldCluster;
                for (uint g = 0u; g < groupsInDraw; ++g)
                {
                    const uint gn = (db.y + g) * 4u;
                    const float4 g0 = manifoldNodes[gn], g1 = manifoldNodes[gn + 1u], g2 = manifoldNodes[gn + 2u];
                    if (!manifoldConeTest(g0.xyz, g1.xyz, g2.xyz, g0.w, pX, L, thetaS)) continue;
                    const uint cEnd = min(clustersInDraw, (g + 1u) * kManifoldCluster);
                    for (uint c = g * kManifoldCluster; c < cEnd; ++c)
                    {
                        ++clusterTests;
                        const uint cn = (header.w + db.x + c) * 4u;
                        const float4 c0 = manifoldNodes[cn], c1 = manifoldNodes[cn + 1u], c2 = manifoldNodes[cn + 2u];
                        const float3 cRef = manifoldNodes[cn + 3u].xyz;   // the cluster's frame of reference
                        if (!manifoldDiscTestTerm(c0.xyz, c1.xyz, c2.xyz, c0.w, c2.w, cRef, target, pX, L)) continue;
                        const uint count = asuint(c1.w);
                        for (uint sc = 0u; sc * 16u < count; ++sc)
                        {
                        ++subTests;
                        const uint sn = (header.w + header.z + header.x + (db.x + c) * 8u + sc) * 4u;
                        const float4 s0 = manifoldNodes[sn], s1 = manifoldNodes[sn + 1u], s2 = manifoldNodes[sn + 2u];
                        if (!manifoldDiscTestTerm(s0.xyz, s1.xyz, s2.xyz, s0.w, s2.w, cRef, target, pX, L)) continue;
                        const uint kEnd = min(count, sc * 16u + 16u);
                        for (uint k = sc * 16u; k < kEnd; ++k)
                        {
                            const uint i = ((db.x + c) * 128u + k) * 9u;
                            ++triTests;
                            const float4 b0 = manifoldTris[i + 6u], b1 = manifoldTris[i + 7u], b2 = manifoldTris[i + 8u];
                            if (!manifoldDiscTestTerm(b0.xyz, b1.xyz, b2.xyz, b0.w, b2.w, cRef, target, pX, L)) continue;
                            ManifoldTri tri;
                            tri.p0 = manifoldTris[i].xyz;
                            tri.e1 = manifoldTris[i + 1u].xyz;
                            tri.e2 = manifoldTris[i + 2u].xyz;
                            tri.n0 = manifoldTris[i + 3u].xyz;
                            tri.a1 = manifoldTris[i + 4u].xyz;
                            tri.a2 = manifoldTris[i + 5u].xyz;
                            tri.c = cRef;
                            ++candidates;
                            if (debugStage == 1u) continue;
                            float2 uv;
                            if (!manifoldSolve(tri, pX, L, target, tol, uv)) continue;
                            if (!(uv.x >= 0.0 && uv.y >= 0.0 && uv.x + uv.y <= 1.0)) continue;
                            float2 sigma, du, dv;
                            float3 y, nS;
                            if (!manifoldMap(tri, pX, L, uv, sigma, du, dv, y, nS)) continue;
                            ++roots;
                            if (debugStage == 2u) continue;
                            // X and the light on opposite sides of the triangle: no reflection there reaches X.
                            const float3 gS = cross(tri.e1, tri.e2);
                            if (dot(gS, pX - y) * dot(gS, L) <= 0.0) continue;
                            const float3 toY = y - pX;
                            const float dist = length(toY);
                            const float3 w = toY / dist;
                            if (dot(w, hx.geoNormal) <= 0.0) continue;
                            // The sun's shadow ray first (an any-hit ray: a root on a solid's far side, or in shadow, is
                            // turned away before the closest-hit trace), from y off its side toward X, as the camera's
                            // light sample leaves its hit.
                            float3 T;
                            if (!visible(y + normalize(gS) * (dot(gS, pX - y) > 0.0 ? 1e-3 : -1e-3), L, 1.0e4, T)) continue;
                            // X -> y as the camera's bounce ray traces it: the first thing it meets must be y.
                            const PathPayload hs = trace(pX, w, kPathMask | kTlasMaskEmitter, 1.0e4);
                            if ((hs.flags & kHit) == 0u || (hs.flags & kHitEmitter) != 0u || hs.material != db.z ||
                                abs(hs.t - dist) > 1e-4 * dist + 1e-5) continue;
                            if (dot(L, hs.geoNormal) <= 0.0) continue;
                            int ch = channel;
                            float3 chMask = float3(1.0);
                            const float4 row13 = materials[hs.material * kMaterialRows + kTransmissionRowFirst + 1u];
                            ManifoldSurface ss = manifoldSurface(hs, -1, nS);
                            if (ss.fromInside) continue;
                            if (ch < 0 && row13.z > 0.0 && ss.mi.transmissionWeight > 0.0 && ss.mi.baseMetalness < 1.0)
                            {
                                // A dispersive surface draws the path's wavelength, as the camera side does there:
                                // that channel alone, three times over (all three at 3 read dispersive glints 3x bright).
                                ch = int(min(manifoldUnit(pcg(h2 ^ k)) * 3.0, 2.0));
                                chMask = float3(ch == 0 ? 3.0 : 0.0, ch == 1 ? 3.0 : 0.0, ch == 2 ? 3.0 : 0.0);
                            }
                            if (ch >= 0) ss = manifoldSurface(hs, ch, nS);
                            const float3 woS = toLocal(-w, ss.t, ss.b, ss.n), wiS = toLocal(L, ss.t, ss.b, ss.n);
                            const float3 core =
                                OpenPBRSurface(ss.mi).evalSmoothCore(woS, wiS, tables, kCausticRough, coreSlope);
                            if (!any(core > 0.0)) continue;
                            float3 sm, ro;
                            bx.evalParts(wo, toLocal(w, sx.t, sx.b, sx.n), tables, kCausticRough, sm, ro);
                            const float3 e12 = cross(tri.e1, tri.e2);
                            const float area2 = length(e12);
                            const float cosY = abs(dot(e12, w)) / area2;
                            const float det = abs(du.x * dv.y - dv.x * du.y);
                            const float dens = manifoldSlopeDensity(alphas, coreSlope, sigma);
                            if (!(det > 0.0) || !(dens > 0.0)) continue;
                            const float3 c = beta * ro * core * E * T * chMask * (invP * cosY * area2 / (dist * dist * det * dens));
                            if (all(isfinite(c)))
                            {
                                sum += c;
                                ++connections;
                            }
                        }
                        }
                    }
                }
                // Step 2b: the same draw, a transmissive solid, refracting the sun through itself onto X. The walk takes
                // the whole instance, so one draw of each (instance, material) runs it (db.w, the host's mark: a merged
                // model's runs of one glass are several draws), with that draw's own choice probability. Not for a solid
                // that cannot refract (metalness 1, thin-walled), nor for a coat or a specular-weight texture (the walk's
                // single interface and IOR would not be the BSDF's: manifoldGlass).
                if ((flags & kFlagManifoldGlass) != 0u && alphas.x > 0.0 && db.w != 0u)
                {
                    const uint mrow = db.z * kMaterialRows;
                    const float4 row12 = materials[mrow + kTransmissionRowFirst];
                    const float4 row13 = materials[mrow + kTransmissionRowFirst + 1u];
                    const bool single = materials[mrow + kLayerRowFirst].w <= 0.0 && asuint(materials[mrow + 5u].w) >= 4096u;
                    if (row12.w > 0.0 && row13.y <= 0.5 && materials[mrow + 1u].y < 1.0 && single)
                        sum += beta * invP *
                               manifoldGlass(da.w, db.z, asfloat(manifoldDraws[3u + 3u * d]), alphas.x, pX, hx.geoNormal,
                                             -rayDir, sx, tables, L, E, channel, pcg(h2 ^ 0x6B1A55u), glassCounts);
                }
            }
        }
        manifoldRequests[r] = float4(0.0);
    }
    if (requests > 0u)
    {
        InterlockedAdd(causticStats[1], requests);
        InterlockedAdd(causticStats[2], candidates);
        InterlockedAdd(causticStats[3], roots);
        InterlockedAdd(causticStats[4], connections);
        InterlockedAdd(causticStats[5], clusterTests);
        InterlockedAdd(causticStats[6], subTests);
        InterlockedAdd(causticStats[7], triTests);
        if (any(glassCounts > 0u))
        {
            InterlockedAdd(causticStats[8], glassCounts.x);
            InterlockedAdd(causticStats[9], glassCounts.y);
            InterlockedAdd(causticStats[10], glassCounts.z);
            InterlockedAdd(causticStats[11], glassCounts.w);
        }
    }
    sum *= classKeep(classMask, kClassManifold);
    if (any(sum != 0.0))
    {
        float4 acc = accumBuffer[pixel];
        acc.rgb += sum / max(acc.w, 1.0);
        accumBuffer[pixel] = acc;
        outputImage[pixel] = float4(displayPixel(acc.rgb, pc.tuning.z, asuint(pc.tuning.w)), 1.0);
    }
}

// Whether the camera sees p first along its ray (the camera's mask and its pass through culled back
// faces, as raygen's camera ray): the light pass connects only where the camera ray would meet p.
bool causticSeen(float3 camPos, float3 p)
{
    const float3 v = p - camPos;
    const float d = length(v);
    const float3 w = v / d;
    float3 o = camPos;
    float travelled = 0.0;
    for (uint k = 0u; k <= kCullSkips; ++k)
    {
        const PathPayload h = trace(o, w, kRayMaskCamera, d * 1.001 + 1e-3 - travelled);
        if ((h.flags & kHit) == 0u) return false;
        if ((h.flags & kHitCulled) != 0u && k < kCullSkips)
        {
            travelled += h.t;
            o = h.position;
            continue;
        }
        return abs(travelled + h.t - d) <= 1e-3 * d + 1e-3;
    }
    return false;
}

// Caustics' light pass (sub-project 24, docs/specs/2026-11-03-caustics-design.md, "The light pass"):
// rays from each directional light, aimed at the casters' disks (rt/caustics.slang), follow smooth parts
// only and, past at least one smooth bounce, connect to the camera through the rough part of each
// surface they reach, splatting onto its pixel. Every weight is in the camera side's roles (OpenPBR is
// not reciprocal): a vertex the light reaches along wl (toward the light) and leaves along we weighs
// f_smooth(wo = we, wi = wl) |we.Ng| / (|wl.Ng| pdf), which carries Veach's shading-normal correction;
// the connection f_rough(wo = to the camera, wi = wl) |wc.Ng| / (|wl.Ng| d^2) x the pixel's importance.
// A pane passes light straight, times its transmission, as shadow rays take it (ruling O14): a null
// event, never a vertex.
[shader("raygeneration")]
void causticTrace()
{
    const uint3 id = DispatchRaysIndex();
    const uint3 dims = DispatchRaysDimensions();
    const uint2 size = pc.params.xy;
    const uint index = (id.z * dims.y + id.y) * dims.x + id.x;
    const float paths = float(dims.x) * float(dims.y) * float(dims.z);
    WalkRng r;
    // render/rt/caustic_casters.cpp's causticPathSeed: the index hashed before it meets the frame's hash (as
    // index ^ hash(frame) a frame's paths filled one block of the seed space, which another frame's often equalled).
    r.state = pcg(pcg(index) ^ pcg(pc.params.z ^ 0xC4057C1Bu));
    // Each draw its own statement: four mutating calls in one constructor came back as one value (the
    // origins then lay on a spiral, not the disk: half the light missing).
    const float o0 = r.next();
    const float o1 = r.next();
    const float o2 = r.next();
    const float o3 = r.next();
    CausticOrigin start = causticOrigin(causticCasters, float4(o0, o1, o2, o3));
    if (!start.valid) return;

    OpenPBRTables tables;
    tables.data = openpbrTables;
    const float3 camPos = pc.invView[3].xyz;
    const float3 fwd = -pc.invView[2].xyz;
    const float3 right = pc.invView[0].xyz;
    const float3 up = pc.invView[1].xyz;
    const float tanY = abs(pc.invProj[1].y);
    const float tanX = tanY * float(size.x) / float(size.y);
    const uint bounces = pc.params.w & 0xFFFFu;

    float3 origin = start.origin;
    float3 dir = start.dir;
    float3 beta = start.weight;
    const float beta0 = max(beta.r, max(beta.g, beta.b));
    uint smoothBounces = 0u;   // the chain's smooth vertices so far (a pane's straight pass is none)
    uint crossings = 0u;       // glass events (refractions, reflections inside), as raygen counts them
    uint firstCross = 0u;      // whether the light's first smooth vertex was one (raygen never counts it:
                               // that vertex is the last of its path, where it samples the light)
    bool insideVolume = false;
    float3 sigmaA = float3(0.0);
    int channel = -1;
    float tMin = 1e-4;
    for (uint k = 0u; k < bounces + kCrossings + 64u; ++k)
    {
        // Until the first smooth bounce the ray stands for the camera side's sun shadow ray (visible(): no emitter
        // meshes, panes passed); after it, for a bounce ray, which an emitter mesh stops.
        const PathPayload hit = trace(origin, dir, smoothBounces == 0u ? kPathMask : (kPathMask | kTlasMaskEmitter), 1.0e4, tMin);
        tMin = 1e-4;
        if ((hit.flags & kHit) == 0u || (hit.flags & kHitEmitter) != 0u) break;
        if (insideVolume) beta *= exp(-sigmaA * hit.t);

        MaterialInputs mi = inputsOf(hit);
        const bool transmissive = mi.transmissionWeight > 0.0 && mi.baseMetalness < 1.0;
        const bool fromInside = transmissive && !mi.thinWalled && (hit.flags & kHitBack) != 0u;
        const float3 n = fromInside ? -hit.normal : hit.normal;
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
        const float3 wl = toLocal(-dir, t, b, n);
        const float4 row13 = materials[hit.material * kMaterialRows + kTransmissionRowFirst + 1u];
        if (transmissive && row13.z > 0.0)
        {
            if (channel < 0)
            {
                channel = int(min(r.next() * 3.0, 2.0));
                beta *= float3(channel == 0 ? 3.0 : 0.0, channel == 1 ? 3.0 : 0.0, channel == 2 ? 3.0 : 0.0);
            }
            mi.specularIor = dispersedIor(mi.specularIor, row13.w, row13.z, kDispersionLambda[channel]);
        }
        const OpenPBRSurface base = OpenPBRSurface(mi);
        const float3 ng = hit.geoNormal;   // facing the side the light came from
        const float cosL = max(abs(dot(dir, ng)), 1e-6);

        // Connect: a term of the light pass ends at a rough part the camera sees, after a smooth bounce,
        // and only a term raygen's path would reach: its j-th vertex is processed while j minus the
        // crossings at vertices before it is at most `bounces` (and those crossings at most kCrossings),
        // which over the chain is largest at its last vertex, the light's first.
        if (smoothBounces > 0u && smoothBounces - (crossings - firstCross) <= bounces && crossings - firstCross <= kCrossings)
        {
            const float3 v = hit.position - camPos;
            const float zc = dot(v, fwd);
            if (zc > 0.0)
            {
                const float2 ndc = float2(dot(v, right) / (zc * tanX), -dot(v, up) / (zc * tanY));
                const float2 pix = (ndc * 0.5 + 0.5) * float2(size) - pc.jitter.xy;
                if (all(pix >= 0.0) && all(pix < float2(size)))
                {
                    const float d = length(v);
                    const float3 wc = -v / d;
                    float3 sm, ro;
                    base.evalParts(toLocal(wc, t, b, n), wl, tables, kCausticRough, sm, ro);
                    if (any(ro > 0.0) && causticSeen(camPos, hit.position))
                    {
                        const float3 value = beta * ro * abs(dot(wc, ng)) / (cosL * d * d) *
                                             causticPixelImportance(zc / d, tanX, tanY, size) / paths;
                        const uint splatBase = (uint(pix.y) * size.x + uint(pix.x)) * 3u;   // uint words: no float atomic
                        [unroll] for (uint rgbIx = 0u; rgbIx < 3u; ++rgbIx)
                        {
                            bool clamped;
                            const uint q = causticQuantize(value[rgbIx], r.next(), clamped);
                            uint before;
                            if (q > 0u) InterlockedAdd(causticSplat[splatBase + rgbIx], q, before);
                            else before = 0u;
                            if (clamped || before > 0xFFFFFFFFu - q) InterlockedAdd(causticStats[0], 1u);
                        }
                    }
                }
            }
        }

        // On through the smooth part, or straight through a pane.
        const float3 shares = base.smoothShares(wl, tables, kCausticRough);
        const float u0 = r.next();
        if (mi.thinWalled && transmissive && paneMaterial(hit.material) && u0 >= shares.x)
        {
            if (!(shares.y > 0.0)) break;
            const float cs = clamp(cosL, 1e-4, 1.0);
            beta *= base.transmissionAlbedo(float3(sqrt(1.0 - cs * cs), 0.0, cs), tables) / shares.y;
            origin = hit.position;
            tMin = 1e-3;
            continue;
        }
        const float u1 = r.next();
        const float u2 = r.next();
        const BsdfSample s = base.sampleSmoothDirection(wl, float3(u0, u1, u2), tables, kCausticRough);
        if (!(s.pdf > 0.0)) break;
        float3 sm, ro;
        base.evalParts(s.wi, wl, tables, kCausticRough, sm, ro);
        const float3 next = toWorld(s.wi, t, b, n);
        beta *= sm * abs(dot(next, ng)) / (cosL * s.pdf);
        if (!all(isfinite(beta)) || !any(beta > 0.0)) break;
        const bool crosses = dot(next, ng) < 0.0;
        ++smoothBounces;
        if (crosses || fromInside)
        {
            ++crossings;
            if (smoothBounces == 1u) firstCross = 1u;
        }
        if (smoothBounces - (crossings - firstCross) > bounces || crossings - firstCross > kCrossings) break;
        origin = hit.position + (crosses ? -ng : ng) * 1e-3;
        dir = next;
        if (crosses && !mi.thinWalled)
        {
            insideVolume = !fromInside;
            sigmaA = float3(0.0);
            if (insideVolume && row13.x > 0.0)
            {
                const float3 c = materials[hit.material * kMaterialRows + kTransmissionRowFirst].rgb;
                sigmaA = -log(max(c, float3(1e-4))) / row13.x;
            }
        }
        if (k >= 2u)
        {
            const float q = min(max(beta.r, max(beta.g, beta.b)) / beta0, 0.95);
            if (r.next() >= q) break;
            beta /= q;
        }
    }
}

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
        // As tracePixel's vertex: the surface seen from wo once, every direction drawn first, then each weighed at
        // one site (OpenPBR inlined once).
        const bool back = base.thinBack(wo);
        const float3 o = back ? base.mirrored(wo) : wo;
        const OpenPBRView surfView = base.view(o, tables);
        const float4 shares = base.drawShares(o, surfView, tables);

        rng.group(1u + 3u * depth);
        const float lu0 = rng.next(), lu1 = rng.next(), lu2 = rng.next();
        const LightSample ls = sampleLight(hit.position, p, lu0, lu1, lu2);
        const bool envOn = envSampled();
        float pdfL = 0.0;
        float3 L = float3(0.0, 0.0, 1.0);
        if (envOn)
        {
            rng.group(2u + 3u * depth);
            L = sampleEnv(rng, pdfL);
        }
        rng.group(3u + 3u * depth);
        BsdfSample s = bsdfSampleNone();
        bool baseDrawn;
        const bool bounce = base.sampleDirectionFrom(o, float3(rng.next(), rng.next(), rng.next()), surfView, shares, tables,
                                                     s, baseDrawn);
        if (back) s.wi = base.mirrored(s.wi);

        // k = 0 the light's, 1 the environment's, 2 the bounce's, every term before the shadow ray (tracePixel's order).
        float3 nee0 = float3(0.0), nee1 = float3(0.0);   // the light's and the environment's light before visibility
        bool shadow0 = false, shadow1 = false;
        [loop]
        for (uint k = 0u; k < 3u; ++k)
        {
            if (k == 0u ? !ls.valid : k == 1u ? !envOn : !bounce) continue;
            const float3 Lw = k == 0u ? ls.L : L;
            const float3 wi = k == 2u ? s.wi : toLocal(Lw, t, b, n);
            const float3 iFront = back ? base.mirrored(wi) : wi;
            float3 sT, cT, dT;
            base.terms(o, iFront, surfView, tables, sT, cT, dT);
            const float3 f = sT + cT + dT;
            if (k < 2u && !((k == 0u || pdfL > 0.0) && any(f > 0.0) && dot(Lw, hit.geoNormal) > 0.0)) continue;
            float bsdfPdf = 0.0, basePdf = 0.0;
            if (k != 0u || (ls.pdf > 0.0 && ls.mis)) bsdfPdf = base.pdfFrom(o, iFront, surfView, shares, tables, basePdf);
            if (k == 2u)
                base.sampleWeigh(s, bsdfPdf, basePdf, baseDrawn, f);
            else if (k == 0u)
            {
                const float w = ls.pdf > 0.0 ? (ls.mis ? powerHeuristic(ls.pdf, bsdfPdf) : 1.0) / ls.pdf : 1.0;
                nee0 = throughput * f * ls.radiance * w;
                shadow0 = true;
            }
            else
            {
                nee1 = throughput * f * envRadiance(L) * powerHeuristic(pdfL, bsdfPdf) / pdfL;
                shadow1 = true;
            }
        }
        [loop]
        for (uint k = 0u; k < 2u; ++k)
        {
            if (!(k == 0u ? shadow0 : shadow1)) continue;
            float3 vis = float3(1.0);
            if (visible(p, k == 0u ? ls.L : L, k == 0u ? ls.dist : 1.0e4, vis))
                cacheContribute(verts, count, (k == 0u ? nee0 : nee1) * vis);
        }
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

[shader("closesthit")]
void closestHit(inout PathPayload payload, BuiltInTriangleIntersectionAttributes attr)
{
    fillHitAt(payload, hitContextHere(attr.barycentrics));
}

[shader("anyhit")]
void anyHit(inout PathPayload payload, BuiltInTriangleIntersectionAttributes attr)
{
    float3 transmission = payload.emission;
    const bool keep = anyHitAccepts(hitContextHere(attr.barycentrics), (payload.flags & kShadowRay) != 0u, transmission);
    // Written before IgnoreHit, which ends the shader: a pane's transmission must ride on with the shadow ray.
    payload.emission = transmission;
    if (!keep) IgnoreHit();
}

// The hit's context from the hit shader's intrinsics.
HitContext hitContextHere(float2 bary)
{
    HitContext h;
    h.instance = InstanceID();
    h.primitive = PrimitiveIndex();
    h.bary = bary;
    h.origin = WorldRayOrigin();
    h.dir = WorldRayDirection();
    h.t = RayTCurrent();
    h.objectToWorld = (float3x3)ObjectToWorld3x4();
    h.worldToObject = (float3x3)WorldToObject3x4();
    h.launch = DispatchRaysIndex().xy;
    return h;
}

[shader("miss")]
void miss(inout PathPayload payload)
{
    payload.flags = 0u;
}
