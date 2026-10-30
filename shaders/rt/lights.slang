// The lights both renderers sample (frontier F4): the analytic lights the path tracer's light table
// holds (render/scene/light_table: every light but flashes, 5 rows a light) and the emissive
// triangles (render/rt/emissive_lights), chosen between by power. rt/path_tracer.slang and the
// deferred frame's direct light (rt/di_*.slang) import it; it owns these bindings of set 0:
//   10 materials, 11 lightData, 12 textures (the bindless table), 42-45 the emissive triangle lights.
import shared.layout;

[[vk::binding(10, 0)]] StructuredBuffer<float4> materials;    // kMaterialRows a material
[[vk::binding(11, 0)]] StructuredBuffer<float4> lightData;    // header, then 5 rows a light
[[vk::binding(12, 0)]] Sampler2D textures[4096];
// The emissive triangle lights (render/rt/emissive_lights.hpp, built by emissive.lights each frame).
[[vk::binding(42, 0)]] StructuredBuffer<uint4> emissiveLights;    // 4 rows a light (bits: the material an index)
[[vk::binding(43, 0)]] Texture2D<float> emissivePower;            // power by light, every level summed 2x2
[[vk::binding(44, 0)]] StructuredBuffer<uint4> emissiveRanges;    // [0] (N, S, levels, R), [1].x the analytic lights' power (bits)
[[vk::binding(45, 0)]] StructuredBuffer<uint> emissiveFirst;      // per instance slot: its first light, ~0 none

static const float kPi = 3.14159265358979;
static const float kTwoPi = 6.28318530717959;

void basisOf(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;   // Duff et al. 2017
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---- lights ---------------------------------------------------------------------------------

uint lightCount() { return asuint(lightData[0].x); }
uint envTexture() { return asuint(lightData[0].y); }
float envScale() { const float s = lightData[0].z; return s > 0.0 ? s : 1.0; }
bool alphaRays() { return asuint(lightData[0].w) == 1u; }

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

// ---- the emissive triangle lights ------------------------------------------------------------
// Light selection by power (PBRT's power light sampler): the analytic lights (uniformly among them)
// with probability q = their power over all the power, the emissive triangles (by power) with 1 - q.

uint emissiveCount() { return emissiveRanges[0].x; }
float analyticShare()
{
    if (lightCount() == 0u) return 0.0;
    if (emissiveCount() == 0u) return 1.0;
    const float analytic = asfloat(emissiveRanges[1].x);
    const float emissive = emissivePower.Load(int3(0, 0, int(emissiveRanges[0].z) - 1));
    return analytic + emissive > 0.0 ? clamp(analytic / (analytic + emissive), 0.0, 1.0) : 0.5;
}

struct TriangleLight
{
    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
    uint material;
};

TriangleLight triangleLight(uint i)
{
    const uint4 a = emissiveLights[i * 4u];
    const float4 b = asfloat(emissiveLights[i * 4u + 1u]), c = asfloat(emissiveLights[i * 4u + 2u]);
    const float4 d = asfloat(emissiveLights[i * 4u + 3u]);
    TriangleLight t;
    t.p0 = asfloat(a.xyz);
    t.p1 = b.xyz;
    t.p2 = c.xyz;
    t.material = a.w;
    t.uv0 = float2(b.w, c.w);
    t.uv1 = d.xy;
    t.uv2 = d.zw;
    return t;
}

// The probability the selection draws emissive light i: (1 - q) x its power over the total.
float emissiveSelect(uint i)
{
    const uint S = emissiveRanges[0].y, levels = emissiveRanges[0].z;
    const float total = emissivePower.Load(int3(0, 0, int(levels) - 1));
    if (!(total > 0.0)) return 0.0;
    return (1.0 - analyticShare()) * emissivePower.Load(int3(int(i % S), int(i / S), 0)) / total;
}

// The density (solid angle) light sampling had for the direction from `origin` to `x` on light i.
float emissivePdf(uint i, float3 origin, float3 x)
{
    const TriangleLight t = triangleLight(i);
    const float3 c = cross(t.p1 - t.p0, t.p2 - t.p0);
    const float area = 0.5 * length(c);
    const float3 toX = x - origin;
    const float d2 = dot(toX, toX);
    const float cosL = abs(dot(normalize(c), toX)) / sqrt(max(d2, 1e-12));
    if (!(area > 0.0) || cosL < 1e-6) return 0.0;
    return emissiveSelect(i) * d2 / (area * cosL);
}

// A triangle drawn by power: `u` descends the power image's levels, choosing among four children in
// proportion (RTXDI's hierarchical draw). False when nothing has power.
bool drawEmissive(float u, out uint i)
{
    i = 0u;
    const uint N = emissiveCount(), S = emissiveRanges[0].y, levels = emissiveRanges[0].z;
    uint2 at = uint2(0u, 0u);
    for (int l = int(levels) - 1; l > 0; --l)
    {
        const uint2 b = at * 2u;
        const float w0 = emissivePower.Load(int3(int2(b), l - 1));
        const float w1 = emissivePower.Load(int3(int2(b + uint2(1u, 0u)), l - 1));
        const float w2 = emissivePower.Load(int3(int2(b + uint2(0u, 1u)), l - 1));
        const float w3 = emissivePower.Load(int3(int2(b + uint2(1u, 1u)), l - 1));
        const float sum = w0 + w1 + w2 + w3;
        if (!(sum > 0.0)) return false;
        float t = min(u, 0.99999994) * sum;
        if (t < w0) { at = b; u = t / w0; }
        else if ((t -= w0) < w1) { at = b + uint2(1u, 0u); u = t / w1; }
        else if ((t -= w1) < w2) { at = b + uint2(0u, 1u); u = t / w2; }
        else { at = b + uint2(1u, 1u); u = min((t - w2) / max(w3, 1e-30), 0.99999994); }
    }
    i = at.y * S + at.x;
    return i < N;
}

// The emissive radiance of material `material` at uv: emission_color x emission_luminance, times the
// emissive texture there.
float3 emissionOf(uint material, float2 uv)
{
    const uint m = material * kMaterialRows;
    float3 le = materials[m + 6u].rgb * materials[m + 2u].y;
    const uint emissiveTex = asuint(materials[m + 1u].w);
    if (emissiveTex < 4096u) le *= textures[NonUniformResourceIndex(emissiveTex)].SampleLevel(uv, 0.0).rgb;
    return le;
}

// A triangle drawn by power (drawEmissive), then a point uniformly on it; its emission with its
// texture there, two-sided as the closest hit's.
LightSample sampleEmissive(float3 p, float u, float u1, float u2)
{
    LightSample s;
    s.valid = false;
    s.mis = true;
    s.pdf = 0.0;
    s.dist = 0.0;
    s.L = float3(0.0, 1.0, 0.0);
    s.radiance = float3(0.0);
    uint i;
    if (!drawEmissive(u, i)) return s;
    const TriangleLight tl = triangleLight(i);
    const float su = sqrt(u1);
    const float b0 = 1.0 - su, b1 = u2 * su, b2 = 1.0 - b0 - b1;
    const float3 x = tl.p0 * b0 + tl.p1 * b1 + tl.p2 * b2;
    const float2 uv = tl.uv0 * b0 + tl.uv1 * b1 + tl.uv2 * b2;
    const float3 c = cross(tl.p1 - tl.p0, tl.p2 - tl.p0);
    const float area = 0.5 * length(c);
    const float3 toX = x - p;
    const float d2 = dot(toX, toX);
    const float d = sqrt(d2);
    if (!(area > 0.0) || !(d > 0.0)) return s;
    s.L = toX / d;
    const float cosL = abs(dot(normalize(c), s.L));
    if (cosL < 1e-6) return s;
    const float3 le = emissionOf(tl.material, uv);
    s.radiance = le;
    s.pdf = emissiveSelect(i) * d2 / (area * cosL);
    s.dist = d * (1.0 - 1e-4) - 1e-3;   // short of the triangle itself
    s.valid = s.pdf > 0.0 && any(le > 0.0);
    return s;
}

// `p` the shaded point; `origin` where its shadow ray starts (p off the surface): an emissive
// triangle is sampled from there, so the shadow ray ends short of the very point it aims at (a plane
// seen at a grazing angle is millimetres nearer from the offset origin than from p).
LightSample sampleLight(float3 p, float3 origin, float u0, float u1, float u2)
{
    LightSample s;
    s.valid = false;
    s.mis = false;
    s.pdf = 0.0;
    s.dist = 0.0;
    s.L = float3(0.0, 1.0, 0.0);
    s.radiance = float3(0.0);
    const uint n = lightCount();
    const float q = analyticShare();
    if (!(u0 < q)) return emissiveCount() > 0u ? sampleEmissive(origin, (u0 - q) / (1.0 - q), u1, u2) : s;
    const uint i = min(uint(u0 / q * float(n)), n - 1u);
    const float choose = q / float(n);
    const Light l = loadLight(i);
    s.mis = l.emitter;
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
    const float choose = analyticShare() / float(lightCount());
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


// ---- light points (the deferred frame's resampling, rt/restir_di.slang) ----------------------
// A reservoir keeps a point on a light, not a direction, so a neighbour re-evaluates the same point
// from where it stands. `light` is an analytic light's index, kLightEmissive | an emissive triangle's,
// or kNoLight; `y` the point (a sun: the direction toward it).

static const uint kNoLight = 0xFFFFFFFFu;
static const uint kLightEmissive = 0x80000000u;

// A light point for p drawn as sampleLight chooses its light, then a point on it: a sphere's near
// side along a direction uniform in its cone seen from p, a point uniform on a rectangle or a
// triangle, a sun's direction. `pdf` is its density in that light's measure: area for every light
// but a sun, whose density is its choice's probability (a delta).
bool drawLightPoint(float3 p, float u0, float u1, float u2, out uint light, out float3 y, out float pdf)
{
    light = kNoLight;
    y = float3(0.0);
    pdf = 0.0;
    const uint n = lightCount();
    const float q = analyticShare();
    if (!(u0 < q))
    {
        uint i;
        if (emissiveCount() == 0u || !drawEmissive((u0 - q) / (1.0 - q), i)) return false;
        const TriangleLight t = triangleLight(i);
        const float su = sqrt(u1);
        const float b0 = 1.0 - su, b1 = u2 * su, b2 = 1.0 - b0 - b1;
        y = t.p0 * b0 + t.p1 * b1 + t.p2 * b2;
        const float area = 0.5 * length(cross(t.p1 - t.p0, t.p2 - t.p0));
        if (!(area > 0.0)) return false;
        light = kLightEmissive | i;
        pdf = emissiveSelect(i) / area;
        return pdf > 0.0;
    }
    const uint i = min(uint(u0 / q * float(n)), n - 1u);
    const float choose = q / float(n);
    const Light l = loadLight(i);
    light = i;
    if (l.type == 1u)
    {
        y = -l.direction;
        pdf = choose;
        return true;
    }
    if (l.type == 3u)
    {
        y = l.position + l.edge1 * u1 + l.edge2 * u2;
        pdf = l.area > 0.0 ? choose / l.area : 0.0;
        return pdf > 0.0;
    }
    const float3 toC = l.position - p;
    const float d2 = dot(toC, toC);
    const float r = l.radius;
    const float sin2 = r * r / d2;
    if (sin2 >= 1.0) return false;
    const float oneMinusCosMax = sin2 / (1.0 + sqrt(1.0 - sin2));
    const float oneMinusCos = u1 * oneMinusCosMax;
    const float cosT = 1.0 - oneMinusCos;
    const float sinT = sqrt(max(0.0, oneMinusCos * (2.0 - oneMinusCos)));
    const float phi = kTwoPi * u2;
    const float d = sqrt(d2);
    const float3 w = toC / d;
    float3 t, b;
    basisOf(w, t, b);
    const float3 L = normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + w * cosT);
    const float dc = d * cosT;
    const float hit = max(dc - sqrt(max(r * r - d2 + dc * dc, 0.0)), 0.0);   // the sphere's near side
    y = p + L * hit;
    const float cosL = dot(normalize(y - l.position), -L);
    if (!(cosL > 0.0) || !(hit > 0.0)) return false;
    pdf = choose / (kTwoPi * oneMinusCosMax) * cosL / (hit * hit);
    return pdf > 0.0;
}

// What arrives at x from light point (light, y): the direction toward it, the distance a shadow ray
// goes (short of the light), the emitted radiance toward x, and the geometry term in the point's
// measure (area: cos at the light over the squared distance; a sun: 1). False when nothing does (the
// far side of a sphere, the back of a rectangle, a light that is gone).
bool arriveFrom(uint light, float3 y, float3 x, out float3 L, out float dist, out float3 le, out float g)
{
    L = float3(0.0, 1.0, 0.0);
    dist = 0.0;
    le = float3(0.0);
    g = 0.0;
    if (light == kNoLight) return false;
    if ((light & kLightEmissive) != 0u)
    {
        const uint i = light & ~kLightEmissive;
        if (i >= emissiveCount()) return false;
        const TriangleLight t = triangleLight(i);
        const float3 e1 = t.p1 - t.p0, e2 = t.p2 - t.p0;
        const float3 c = cross(e1, e2);
        const float len = length(c);
        const float3 toY = y - x;
        const float d2 = dot(toY, toY);
        const float d = sqrt(d2);
        if (!(len > 0.0) || !(d > 0.0)) return false;
        L = toY / d;
        const float cosL = abs(dot(c / len, L));
        if (cosL < 1e-6) return false;
        // y's barycentrics, for the emissive texture there.
        const float3 v = y - t.p0;
        const float d00 = dot(e1, e1), d01 = dot(e1, e2), d11 = dot(e2, e2);
        const float d20 = dot(v, e1), d21 = dot(v, e2);
        const float den = d00 * d11 - d01 * d01;
        const float b1 = den != 0.0 ? (d11 * d20 - d01 * d21) / den : 0.0;
        const float b2 = den != 0.0 ? (d00 * d21 - d01 * d20) / den : 0.0;
        le = emissionOf(t.material, t.uv0 * (1.0 - b1 - b2) + t.uv1 * b1 + t.uv2 * b2);
        g = cosL / d2;
        dist = d * (1.0 - 1e-4) - 1e-3;
        return any(le > 0.0);
    }
    if (light >= lightCount()) return false;
    const Light l = loadLight(light);
    if (l.type == 1u)
    {
        L = normalize(y);
        dist = 1.0e4;
        le = l.radiance;
        g = 1.0;
        return any(le > 0.0);
    }
    const float3 toY = y - x;
    const float d2 = dot(toY, toY);
    const float d = sqrt(d2);
    if (!(d > 0.0)) return false;
    L = toY / d;
    float cosL;
    if (l.type == 3u)
    {
        cosL = dot(-L, normalize(cross(l.edge1, l.edge2)));
        le = l.radiance / (4.0 * kPi * l.radius * l.radius);
    }
    else
    {
        cosL = dot(normalize(y - l.position), -L);
        le = sphereRadiance(l, x);
    }
    if (!(cosL > 0.0)) return false;
    g = cosL / d2;
    dist = d - 1e-3;
    return any(le > 0.0);
}
