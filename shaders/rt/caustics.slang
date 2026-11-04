// Caustics' light pass (sub-project 24, docs/specs/2026-11-03-caustics-design.md, "The light pass"):
// where a ray from a directional light starts, the camera's importance for a pixel, and the fixed-point
// splat. rt/path_tracer.slang's causticTrace calls these; render/rt/caustic_casters.cpp is their CPU
// twin (the same draw and density, held by caustic_casters_test), render/rt/caustic_pass.cpp writes the
// table.
//
// The table (StructuredBuffer<float4>): row 0 (lights, disks, the scene's radius, 0), row 1 (the scene's
// centre); then three rows a light: (the direction it travels, its irradiance's luminance), (its
// irradiance, the cumulative probability of choosing it), (its disks' area, its first disk, its disk
// count, 0); then two rows a disk: (centre, radius), (the cumulative share of its light's area, 0, 0, 0).
import shared.layout;

// An orthonormal pair perpendicular to n (Duff et al. 2017), as causticBasis in C++.
void causticBasis(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

struct CausticOrigin
{
    float3 origin;   // upstream of the whole scene
    float3 dir;      // the light's direction of travel
    float3 weight;   // the light's irradiance over the origin's density per unit area of its plane
    bool valid;
};

// A ray from the four numbers u: a light by its power, a disk of it by area, a point uniformly on the
// disk. Its density is p(light) x (the light's disks covering the point) / the light's disk area.
CausticOrigin causticOrigin(StructuredBuffer<float4> table, float4 u)
{
    CausticOrigin o;
    o.origin = float3(0.0);
    o.dir = float3(0.0, -1.0, 0.0);
    o.weight = float3(0.0);
    o.valid = false;
    const float4 h = table[0];
    const uint lights = asuint(h.x);
    if (lights == 0u) return o;
    uint l = 0u;
    while (l + 1u < lights && u.x >= table[3u + 3u * l].w) ++l;
    const float cdfHere = table[3u + 3u * l].w;
    const float cdfBefore = l > 0u ? table[3u + 3u * (l - 1u)].w : 0.0;
    const float pLight = cdfHere - cdfBefore;
    const float4 la = table[2u + 3u * l], lb = table[3u + 3u * l], lc = table[4u + 3u * l];
    const uint first = asuint(lc.y), count = asuint(lc.z);
    if (count == 0u || !(lc.x > 0.0) || !(pLight > 0.0)) return o;
    const float3 dir = normalize(la.xyz);
    const uint disks = 2u + 3u * lights;
    // The disk: the first whose cumulative share exceeds u.y.
    uint lo = first, hi = first + count - 1u;
    while (lo < hi)
    {
        const uint mid = (lo + hi) / 2u;
        if (u.y < table[disks + 2u * mid + 1u].x) hi = mid;
        else lo = mid + 1u;
    }
    const float4 disk = table[disks + 2u * lo];
    float3 t, b;
    causticBasis(dir, t, b);
    const float r = disk.w * sqrt(u.z);
    const float phi = 2.0 * 3.14159265358979 * u.w;
    const float3 p = disk.xyz + t * (r * cos(phi)) + b * (r * sin(phi));
    uint covering = 0u;
    for (uint k = first; k < first + count; ++k)
    {
        const float4 dk = table[disks + 2u * k];
        const float3 v = p - dk.xyz;
        const float3 perp = v - dir * dot(v, dir);
        if (dot(perp, perp) < dk.w * dk.w) ++covering;
    }
    const float density = pLight * float(max(covering, 1u)) / lc.x;
    const float3 centre = table[1].xyz;
    const float radius = h.z;
    o.origin = p - dir * (dot(p - centre, dir) + radius * 1.01 + 1.0);
    o.dir = dir;
    o.weight = lb.rgb / density;
    o.valid = true;
    return o;
}

// The camera's importance per unit solid angle for a box pixel, seen at cosCam from the view axis:
// a pixel's value is the mean radiance over its footprint on the image plane (1 unit away, its sides
// 2 tanX / width and 2 tanY / height), whose solid angle is its area x cos^3.
float causticPixelImportance(float cosCam, float tanX, float tanY, uint2 size)
{
    const float sx = 2.0 * tanX / float(size.x), sy = 2.0 * tanY / float(size.y);
    return 1.0 / (sx * sy * cosCam * cosCam * cosCam);
}

// A splat's channel in fixed point (x kCausticSplatScale), rounded stochastically by u so the mean is
// exact however small the value; over 2^28 it is clamped (and counted by the caller).
uint causticQuantize(float v, float u, out bool clamped)
{
    const float x = v * kCausticSplatScale;
    clamped = !(x >= 0.0) || x > 268435456.0;
    if (!(x >= 0.0)) return 0u;
    return uint(floor(min(x, 268435456.0) + u));
}
