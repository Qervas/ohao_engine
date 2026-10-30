// The deferred frame's direct light (frontier F4): ReSTIR DI (Bitterli et al. 2020, "Spatiotemporal
// reservoir resampling for real-time ray tracing with dynamic direct lighting") over the lights the
// path tracer samples (rt/lights.slang: every analytic light but flashes, and the emissive
// triangles), in MegaLights' structure: sample, trace, shade, then denoise (rt/di_denoise.slang).
//
// initial  Each pixel writes its surface (a compact copy: position, distance from the camera,
//          normal, and the few numbers the target needs), draws `candidates` light points
//          (drawLightPoint: the light by power, a point on it) and keeps one by resampled importance
//          sampling against the target
//            p(y) = luminance(Le(y)) x (kd / pi + ks x D x Vis) x cos x G(y)      unshadowed:
//          Lambert under GGX with the surface's diffuse and specular reflectance (RIS needs a
//          target near the light's shape; W carries the difference, and shading is exact); then
//          last frame's reservoir at the pixel the motion vectors give, where last frame's surface
//          there is the same (distance, normal), its M capped at kDIHistoryCap x candidates; one
//          shadow ray to the chosen point (visibility reuse: occluded, it keeps its M and loses its
//          weight).
// shade    `spatial` neighbours within kDISpatialRadius pixels whose surfaces are the same are
//          combined with the pixel's (the reservoir kept for next frame's temporal reuse); one
//          shadow ray to the final point (alpha cutouts and smoke as RT GI's rays meet them), and
//          its light
//            f x Le x G x V x W        f OpenPBR's (evalLobes)
//          split into the diffuse and specular lobes, each over its directional albedo toward the
//          viewer (demodulated: the denoiser filters light, lighting multiplies the albedos back).
//          Reference quality reuses nothing and keeps the running mean of the frames since the last
//          reset: an unbiased estimate that converges.
//
// Combinations stream (each reservoir weighed by its point's target at this pixel) and normalise by
// Z, the M of the inputs whose own surface could have drawn the chosen point: its target there above
// 0 and the point visible from it (Bitterli et al.'s Algorithm 6 with its visibility rays, RTXDI's
// ray-traced bias correction). 1/M counts a neighbour that sees the far side of a sphere light or is
// in shadow, and darkens a penumbra more with every frame that reuses it. Temporal reuse reads last
// frame's reservoir at the same surface, so the one ray that tests the chosen point here answers for
// both. Every number is a hash of (seed, frame since the reset, pixel, pass): no atomics, no order,
// so a frame repeats bit for bit.
import shared.layout;
import material.openpbr;
import includes.lighting.gbuffer_material;
import rt.lights;
import rt.alpha_test;
import rt.smoke_proxy;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] Texture2D<float4> gPosition;   // xyz, metallic; (0,0,0,0) sky, w < 0 a light's emitter mesh
[[vk::binding(2, 0)]] Texture2D<float4> gNormal;     // octahedral normal, roughness
[[vk::binding(3, 0)]] Texture2D<float4> gAlbedo;     // albedo, AO
[[vk::binding(4, 0)]] Texture2D<float4> gSpecular;   // specular_color, specular_weight / 1.5
[[vk::binding(5, 0)]] Texture2D<float4> gOpenPBR;    // base_diffuse_roughness, (specular_ior - 1) / 2.55
[[vk::binding(6, 0)]] Texture2D<float2> gVelocity;   // UV motion: current minus previous
[[vk::binding(7, 0)]] StructuredBuffer<float> openpbrTables;
// Reservoirs, two RGBA32F images each: A (the point, the light as bits), B (W, M).
[[vk::binding(8, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> prevA;   // last frame's, after reuse
[[vk::binding(9, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> prevB;
[[vk::binding(13, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> midA;   // after the temporal reuse
[[vk::binding(14, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> midB;
[[vk::binding(15, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outA;   // this frame's, after reuse
[[vk::binding(16, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outB;
// The light, demodulated: realtime the noisy frame (the denoiser's input), reference the mean.
[[vk::binding(17, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> noisyDiffuse;
[[vk::binding(18, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> noisySpecular;
[[vk::binding(19, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> meanDiffuse;
[[vk::binding(20, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> meanSpecular;
// Surfaces, two RGBA32F images each: A (position, distance from the camera; 0 none), B (the normal
// as two 16-bit octahedral halves, GGX alpha, diffuse and specular reflectance). This frame's and
// last frame's: the guides of temporal and spatial reuse and of the denoiser.
[[vk::binding(21, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> surfA;
[[vk::binding(22, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> surfB;
[[vk::binding(23, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> prevSurfA;
[[vk::binding(24, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> prevSurfB;
// The denoiser's guide (rt/di_denoise.slang), 8 bytes a pixel: the distance and the packed normal.
[[vk::binding(25, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> guide;
[[vk::push_constant]] ConstantBuffer<DIPush> pc;

static const uint kFlagReference = 1u;
static const uint kFlagHistory = 2u;
static const float kRayOffset = 0.05;   // along the normal, as RT GI's rays start

// ---- random numbers -------------------------------------------------------------------------

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

Rng rngOf(uint2 pixel, uint pass)
{
    Rng r;
    r.state = pcg(pc.extent.w ^ pcg(pc.extent.z ^ pcg(pixel.x ^ pcg(pixel.y ^ pcg(pass)))));
    return r;
}

// ---- the pixel ------------------------------------------------------------------------------

// The full surface, for shading: the G-buffer's material through OpenPBR.
struct Pixel
{
    float3 position;
    float3 normal;
    float3 t, b;
    float3 wo;
    OpenPBRBase bsdf;
    bool valid;      // a surface (not the sky, not a light's emitter mesh)
};

float3 toLocalFrame(float3 v, Pixel s) { return float3(dot(v, s.t), dot(v, s.b), dot(v, s.normal)); }

Pixel pixelAt(uint2 q)
{
    Pixel s;
    const float4 g0 = gPosition.Load(int3(q, 0));
    s.valid = !all(g0 == float4(0.0)) && g0.w >= 0.0;
    s.position = g0.xyz;
    const float4 g1 = gNormal.Load(int3(q, 0));
    s.normal = decodeNormalOctahedron(g1.xy);
    Weather w;
    w.wetness = pc.weather.x;
    w.snowCover = pc.weather.y;
    w.frostCover = pc.weather.z;
    const MaterialInputs mi = gbufferMaterial(g0, g1, gAlbedo.Load(int3(q, 0)), gSpecular.Load(int3(q, 0)),
                                              gOpenPBR.Load(int3(q, 0)).rg, s.normal, w);
    s.bsdf = OpenPBRBase(mi);
    basisOf(s.normal, s.t, s.b);
    // A view below the shading normal (a normal map tilting away) is taken at the horizon.
    float3 wo = toLocalFrame(normalize(pc.cameraPos.xyz - s.position), s);
    wo.z = max(wo.z, 1e-4);
    s.wo = normalize(wo);
    return s;
}

OpenPBRTables tables()
{
    OpenPBRTables t;
    t.data = openpbrTables;
    return t;
}

// The compact surface: what reuse needs of a pixel, its own or a neighbour's, in two loads.
struct Surf
{
    float3 position;
    float dist;      // from the camera; 0 none (the sky, an emitter mesh)
    float3 normal;
    float alpha;     // GGX
    float kd;        // diffuse reflectance (luminance)
    float ks;        // specular reflectance at normal incidence (luminance)
};

uint packNormal(float3 n)
{
    // Octahedral, 16 bits a coordinate.
    float2 e = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) e = (1.0 - abs(e.yx)) * float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    const uint2 q = uint2(saturate(e * 0.5 + 0.5) * 65535.0 + 0.5);
    return q.x | (q.y << 16u);
}

float3 unpackNormal(uint v)
{
    return decodeNormalOctahedron(float2(float(v & 0xFFFFu), float(v >> 16u)) / 65535.0);
}

Surf surfOf(Pixel s)
{
    Surf f;
    f.position = s.position;
    f.dist = s.valid ? max(distance(s.position, pc.cameraPos.xyz), 1e-6) : 0.0;
    f.normal = s.normal;
    f.alpha = s.bsdf.alpha;
    const float dielectricF0 = ((s.bsdf.eta - 1.0) / (s.bsdf.eta + 1.0)) * ((s.bsdf.eta - 1.0) / (s.bsdf.eta + 1.0));
    f.kd = luminance(s.bsdf.rho) * (1.0 - s.bsdf.metalness);
    f.ks = lerp(dielectricF0 * luminance(s.bsdf.specColor), luminance(s.bsdf.metalF0) * s.bsdf.metalWeight,
                s.bsdf.metalness);
    return f;
}

void storeSurf(uint2 q, Surf f)
{
    const float n = asfloat(packNormal(f.normal));
    surfA[q] = float4(f.position, f.dist);
    surfB[q] = float4(n, f.alpha, f.kd, f.ks);
    guide[q] = float2(f.dist, n);
}

Surf loadSurf(RWTexture2D<float4> a, RWTexture2D<float4> b, uint2 q)
{
    const float4 va = a[q], vb = b[q];
    Surf f;
    f.position = va.xyz;
    f.dist = va.w;
    f.normal = unpackNormal(asuint(vb.x));
    f.alpha = vb.y;
    f.kd = vb.z;
    f.ks = vb.w;
    return f;
}

// The same surface: within 5% of the distance and 18 degrees of the normal. Looser, a sample a
// neighbour on another surface saw at a grazing angle comes here head-on with its weight, an outlier
// that temporal and spatial reuse then keep and spread.
bool sameSurface(Surf a, Surf b)
{
    if (!(a.dist > 0.0) || !(b.dist > 0.0)) return false;
    return abs(b.dist - a.dist) <= 0.05 * a.dist && dot(a.normal, b.normal) > 0.95;
}

// The target: what light point (light, y) gives surface s, unshadowed, as a luminance.
float target(Surf s, uint light, float3 y)
{
    if (light == kNoLight || !(s.dist > 0.0)) return 0.0;
    float3 L, le;
    float dist, g;
    if (!arriveFrom(light, y, s.position, L, dist, le, g)) return 0.0;
    const float nl = dot(s.normal, L);
    if (nl <= 0.0) return 0.0;
    const float3 V = normalize(pc.cameraPos.xyz - s.position);
    const float nv = max(dot(s.normal, V), 1e-4);
    const float3 H = normalize(L + V);
    const float nh = saturate(dot(s.normal, H));
    const float a2 = max(s.alpha * s.alpha, 1e-6);
    const float d = nh * nh * (a2 - 1.0) + 1.0;
    const float D = a2 / (kPi * d * d);
    // Smith's height-correlated visibility (Heitz 2014), its approximation without the square roots
    // (Karis): enough for a target.
    const float vis = 0.5 / max(nl * (nv * (1.0 - s.alpha) + s.alpha) + nv * (nl * (1.0 - s.alpha) + s.alpha), 1e-6);
    return luminance(le) * (s.kd * (1.0 / kPi) + s.ks * D * vis) * nl * g;
}

// ---- reservoirs -----------------------------------------------------------------------------

struct Reservoir
{
    uint light;
    float3 y;
    float W;      // the unbiased contribution weight: 1 / p(y) x the weights' mean
    float M;      // the candidates it stands for
};

Reservoir emptyReservoir()
{
    Reservoir r;
    r.light = kNoLight;
    r.y = float3(0.0);
    r.W = 0.0;
    r.M = 0.0;
    return r;
}

// The streaming combination: each input weighed by its point's target here x its W x its M.
struct Combiner
{
    uint light;
    float3 y;
    float pHat;
    float wsum;
    float M;

    [mutating] void add(uint l, float3 p, float W, float m, float pHatHere, float u)
    {
        const float w = pHatHere * W * m;
        M += m;
        if (!(w > 0.0)) return;
        wsum += w;
        if (u * wsum < w)
        {
            light = l;
            y = p;
            pHat = pHatHere;
        }
    }

    // `Z`: the M that counts (see the header); M for the candidates of one pixel.
    Reservoir result(float Z)
    {
        Reservoir r;
        r.light = light;
        r.y = y;
        r.M = M;
        r.W = pHat > 0.0 && Z > 0.0 ? wsum / (Z * pHat) : 0.0;
        if (!(r.W > 0.0)) r.light = kNoLight;
        return r;
    }
};

Combiner emptyCombiner()
{
    Combiner c;
    c.light = kNoLight;
    c.y = float3(0.0);
    c.pHat = 0.0;
    c.wsum = 0.0;
    c.M = 0.0;
    return c;
}

void store(RWTexture2D<float4> a, RWTexture2D<float4> b, uint2 q, Reservoir r)
{
    a[q] = float4(r.y, asfloat(r.light));
    b[q] = float4(r.W, r.M, 0.0, 0.0);
}

Reservoir load(RWTexture2D<float4> a, RWTexture2D<float4> b, uint2 q)
{
    const float4 va = a[q], vb = b[q];
    Reservoir r;
    r.y = va.xyz;
    r.light = asuint(va.w);
    r.W = vb.x;
    r.M = vb.y;
    return r;
}

// ---- visibility -----------------------------------------------------------------------------

// True when no surface stops a ray from `origin` along `dir` for `dist`: alpha cutouts by
// rt/alpha_test.slang, a light's emitter mesh never.
bool unoccluded(float3 origin, float3 dir, float dist)
{
    if (!(dist > 0.0)) return false;
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.001;
    ray.Direction = dir;
    ray.TMax = dist;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
    q.TraceRayInline(scene, 0, kRayMaskScene, ray);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            alphaHit(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_NOTHING;
}

// 1 when the ray is unoccluded and smoke lets it through (with probability its transmittance).
float visibility(float3 origin, float3 dir, float dist, inout Rng rng)
{
    if (!unoccluded(origin, dir, dist)) return 0.0;
    const float T = smokeTransmittance(origin, dir, dist);
    if (T >= 1.0) return 1.0;
    return rng.next() < T ? 1.0 : 0.0;
}

float visibilityOf(Surf s, uint light, float3 y, inout Rng rng)
{
    const float3 origin = s.position + s.normal * kRayOffset;
    float3 L, le;
    float dist, g;
    if (!arriveFrom(light, y, origin, L, dist, le, g)) return 0.0;
    return visibility(origin, L, dist, rng);
}

// Whether surface s could have drawn light point (light, y): its target there above 0, the point
// unoccluded from it (smoke aside: Z counts surfaces, not a draw through a cloud).
bool couldDraw(Surf s, uint light, float3 y)
{
    if (!(target(s, light, y) > 0.0)) return false;
    const float3 origin = s.position + s.normal * kRayOffset;
    float3 L, le;
    float dist, g;
    return arriveFrom(light, y, origin, L, dist, le, g) && unoccluded(origin, L, dist);
}

// ---- initial: candidates, temporal reuse, visibility -----------------------------------------

// The boiling filter (RTXDI's): a reservoir whose estimate (its point's target x W) is over
// kBoilingFactor x the mean of its 8x8 tile's non-zero ones is dropped. In a shadow, RIS against an
// unshadowed target sometimes keeps a dim light's point with the occluded light's weight: unbiased,
// but temporal reuse holds such an outlier for many frames and the denoiser spreads it into a blob.
static const float kBoilingFactor = 20.0;
groupshared float gEstimate[64];
groupshared float gMeanEstimate;

Reservoir initialReservoir(uint2 q, Surf s)
{
    Rng rng = rngOf(q, 0u);

    // Resampled importance sampling over the candidates: each weighed by its target over the
    // density it was drawn with.
    Combiner c = emptyCombiner();
    const uint candidates = clamp(pc.params.x, 1u, kDIMaxCandidates);
    for (uint k = 0u; k < candidates; ++k)
    {
        const float u0 = rng.next(), u1 = rng.next(), u2 = rng.next();
        const float pick = rng.next();
        uint light;
        float3 y;
        float pdf;
        if (!drawLightPoint(s.position, u0, u1, u2, light, y, pdf))
        {
            c.M += 1.0;
            continue;
        }
        c.add(light, y, 1.0 / pdf, 1.0, target(s, light, y), pick);
    }
    Reservoir r = c.result(c.M);

    // Temporal reuse.
    if ((pc.params.z & kFlagReference) == 0u && (pc.params.z & kFlagHistory) != 0u)
    {
        const float2 uv = (float2(q) + 0.5) / float2(pc.extent.xy);
        const float2 prevUv = uv - gVelocity.Load(int3(q, 0));
        const int2 pq = int2(floor(prevUv * float2(pc.extent.xy)));
        if (all(pq >= 0) && all(pq < int2(pc.extent.xy)))
        {
            const Surf ps = loadSurf(prevSurfA, prevSurfB, uint2(pq));
            if (sameSurface(s, ps))
            {
                Reservoir prev = load(prevA, prevB, uint2(pq));
                prev.M = min(prev.M, float(kDIHistoryCap * candidates));
                Combiner t = emptyCombiner();
                t.add(r.light, r.y, r.W, r.M, target(s, r.light, r.y), rng.next());
                t.add(prev.light, prev.y, prev.W, prev.M, target(s, prev.light, prev.y), rng.next());
                // Z: this pixel's candidates if its target is above 0 at the point, last frame's if
                // last frame's surface's is. Visibility is the ray below's.
                float Z = 0.0;
                if (target(s, t.light, t.y) > 0.0) Z += r.M;
                if (target(ps, t.light, t.y) > 0.0) Z += prev.M;
                r = t.result(Z);
            }
        }
    }

    // Visibility reuse: an occluded point keeps its M and loses its weight.
    if (r.W > 0.0 && visibilityOf(s, r.light, r.y, rng) == 0.0)
    {
        r.W = 0.0;
        r.light = kNoLight;
    }
    return r;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void initial(uint3 id : SV_DispatchThreadID, uint li : SV_GroupIndex)
{
    const uint2 q = id.xy;
    const bool inside = all(q < pc.extent.xy);
    Surf s;
    s.dist = 0.0;
    Reservoir r = emptyReservoir();
    float estimate = 0.0;
    if (inside)
    {
        s = surfOf(pixelAt(q));
        storeSurf(q, s);
        if (s.dist > 0.0)
        {
            r = initialReservoir(q, s);
            if (r.light != kNoLight) estimate = r.W * target(s, r.light, r.y);
        }
    }
    gEstimate[li] = estimate;
    GroupMemoryBarrierWithGroupSync();
    if (li == 0u)
    {
        // In a fixed order: the same bits every run.
        float sum = 0.0, n = 0.0;
        for (uint k = 0u; k < 64u; ++k)
        {
            if (gEstimate[k] > 0.0)
            {
                sum += gEstimate[k];
                n += 1.0;
            }
        }
        gMeanEstimate = n > 0.0 ? sum / n : 0.0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (!inside) return;
    if ((pc.params.z & kFlagReference) == 0u && estimate > kBoilingFactor * gMeanEstimate)
    {
        r.W = 0.0;
        r.light = kNoLight;
    }
    store(midA, midB, q, r);
}

// ---- shade: spatial reuse, the final ray, the light ------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void shade(uint3 id : SV_DispatchThreadID)
{
    const uint2 q = id.xy;
    if (any(q >= pc.extent.xy)) return;
    const bool reference = (pc.params.z & kFlagReference) != 0u;
    const Surf s = loadSurf(surfA, surfB, q);
    float3 diffuse = float3(0.0), specular = float3(0.0);
    if (s.dist > 0.0)
    {
        Rng rng = rngOf(q, 1u);
        Reservoir own = load(midA, midB, q);
        Combiner c = emptyCombiner();
        c.add(own.light, own.y, own.W, own.M, target(s, own.light, own.y), rng.next());
        // The neighbours combined, for Z.
        int2 used[kDIMaxSpatial];
        float usedM[kDIMaxSpatial];
        uint usedCount = 0u;
        if (!reference)
        {
            const uint count = min(pc.params.y, kDIMaxSpatial);
            for (uint k = 0u; k < count; ++k)
            {
                // A neighbour uniform in the disc of kDISpatialRadius pixels.
                const float radius = float(kDISpatialRadius) * sqrt(rng.next());
                const float angle = kTwoPi * rng.next();
                const int2 n = int2(q) + int2(round(float2(cos(angle), sin(angle)) * radius));
                const float u = rng.next();
                if (any(n < 0) || any(n >= int2(pc.extent.xy)) || all(n == int2(q))) continue;
                if (!sameSurface(s, loadSurf(surfA, surfB, uint2(n)))) continue;
                const Reservoir r = load(midA, midB, uint2(n));
                c.add(r.light, r.y, r.W, r.M, target(s, r.light, r.y), u);
                used[usedCount] = n;
                usedM[usedCount] = r.M;
                ++usedCount;
            }
        }
        // The shading ray: the chosen point's visibility here.
        float v = 0.0;
        if (c.light != kNoLight) v = visibilityOf(s, c.light, c.y, rng);
        // Z: this pixel's M when it sees the point (else nothing is shaded), and each neighbour's
        // when it could have drawn it: one more ray a neighbour, only when the point is lit here.
        float Z = c.M;
        if (usedCount > 0u && v > 0.0)
        {
            Z = own.M;
            for (uint k = 0u; k < usedCount; ++k)
                if (couldDraw(loadSurf(surfA, surfB, uint2(used[k])), c.light, c.y)) Z += usedM[k];
        }
        Reservoir r = c.result(Z);

        // The light, with the full material: OpenPBR's lobes, over their directional albedos.
        const Pixel p = pixelAt(q);
        if (r.W > 0.0 && v > 0.0)
        {
            float3 L, le;
            float dist, g;
            if (arriveFrom(r.light, r.y, p.position, L, dist, le, g))
            {
                const float3 wi = toLocalFrame(L, p);
                float3 Ls, Ld;
                p.bsdf.evalLobes(p.wo, wi, tables(), Ls, Ld);
                const float3 light = le * (g * r.W * v);
                diffuse = Ld * light / max(p.bsdf.diffuseAlbedo(p.wo, tables()), float3(1e-3));
                specular = Ls * light / max(p.bsdf.specularAlbedo(p.wo, tables()), float3(1e-3));
            }
        }
        // The kept reservoir loses an occluded point's weight (visibility reuse at the end): next
        // frame's temporal reuse and its neighbours' spatial reuse never draw it again.
        if (!(v > 0.0))
        {
            r.W = 0.0;
            r.light = kNoLight;
        }
        store(outA, outB, q, r);
    }
    else
    {
        store(outA, outB, q, emptyReservoir());
    }

    if (reference)
    {
        const float k = float(pc.params.w);
        const float3 md = pc.params.w == 0u ? float3(0.0) : meanDiffuse[q].rgb;
        const float3 ms = pc.params.w == 0u ? float3(0.0) : meanSpecular[q].rgb;
        meanDiffuse[q] = float4(md + (diffuse - md) / (k + 1.0), 1.0);
        meanSpecular[q] = float4(ms + (specular - ms) / (k + 1.0), 1.0);
    }
    else
    {
        noisyDiffuse[q] = float4(diffuse, 1.0);
        noisySpecular[q] = float4(specular, 1.0);
    }
}
