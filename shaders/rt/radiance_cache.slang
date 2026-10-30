// The radiance cache (frontier F1 T3; SHaRC's design, OHAO's code): a world-space hash grid of the
// radiance a surface reflects, which realtime paths end in past their first bounce, so a frame
// traces short paths and still sees every bounce. Sources: Binder et al., "Fast Path Space
// Filtering by Jittered Spatial Hashing" (SIGGRAPH 2019 talks); Gautron, "Real-Time Ray-Traced
// Ambient Occlusion of Complex Scenes using Spatial Hashing" (SIGGRAPH 2020 talks); NVIDIA's
// SharcGuide as the reference design (its code is not ours to ship: docs/specs/
// 2026-10-30-frontier-renderer-design.md, 4a).
//
// A cell is a voxel of a level, its size doubling with the distance from the camera, and the side
// of it a surface faces (the normal's dominant axis and sign: a wall's two sides never share light).
// Its key is a nonzero hash of the three; its slot is found by probing kCacheProbes slots from a
// second hash. Three buffers of `capacity` entries (a power of two):
//   keys      the cell's key, 0 when free;
//   accum     this frame's sum: rgb radiance in units of 1/kCacheScale and the sample count, added
//             by InterlockedAdd (integers: the sum is the same in any order, so captures repeat);
//   resolved  rgb the cell's radiance as float bits, w its sample count (low 16 bits) and the
//             frames since a sample last reached it (high 16).
// A frame: the update pass (path_tracer.slang's cacheUpdate) traces 1 pixel in 5x5 and adds, at
// every vertex, the light that the rest of its path brings back; the resolve pass
// (radiance_cache_resolve.slang) blends it into `resolved` and frees cells stale for long; the
// realtime raygen ends a path in the cell of its second or later vertex.

static const uint kCacheProbes = 8u;
static const float kCacheScale = 1024.0;        // radiance's integer units: 1/1024
static const float kCacheMaxRadiance = 2000.0;  // a sample's radiance is clamped to this (no uint overflow)
static const float kCacheVoxelNear = 0.05;      // a voxel's size within 1 m of the camera (m)
static const uint kCacheMaxLevel = 14u;

uint cacheHash(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

// The level of a point `d` metres from the camera: 0 within 1 m, then one a doubling of distance.
uint cacheLevel(float d) { return min(uint(max(floor(log2(max(d, 1.0))), 0.0)), kCacheMaxLevel); }

float cacheVoxelSize(uint level) { return kCacheVoxelNear * float(1u << level); }

float cacheVoxelSizeAt(float3 p, float3 camera) { return cacheVoxelSize(cacheLevel(length(p - camera))); }

// The side of the voxel a surface of normal n faces: 0-5 (+x, -x, +y, -y, +z, -z).
uint cacheSide(float3 n)
{
    const float3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) return n.x >= 0.0 ? 0u : 1u;
    if (a.y >= a.z) return n.y >= 0.0 ? 2u : 3u;
    return n.z >= 0.0 ? 4u : 5u;
}

// The cell of a point: its key (never 0) and its first slot's hash.
uint2 cacheCell(float3 p, float3 n, float3 camera)
{
    const uint level = cacheLevel(length(p - camera));
    const int3 v = int3(floor(p / cacheVoxelSize(level)));
    uint h = cacheHash(uint(v.x) ^ 0x9E3779B9u);
    h = cacheHash(h ^ uint(v.y));
    h = cacheHash(h ^ uint(v.z));
    h = cacheHash(h ^ (level * 8u + cacheSide(n)));
    const uint key = h | 1u;
    return uint2(key, cacheHash(h ^ 0x68E31DA4u));
}

// The slot holding `key`, or ~0u.
uint cacheFind(RWStructuredBuffer<uint> keys, uint2 cell)
{
    uint capacity, stride;
    keys.GetDimensions(capacity, stride);
    for (uint i = 0u; i < kCacheProbes; ++i)
    {
        const uint slot = (cell.y + i) & (capacity - 1u);
        if (keys[slot] == cell.x) return slot;
    }
    return ~0u;
}

// The slot holding `key`, claimed if no slot holds it yet; ~0u when every probe is taken.
uint cacheInsert(RWStructuredBuffer<uint> keys, uint2 cell)
{
    const uint found = cacheFind(keys, cell);
    if (found != ~0u) return found;
    uint capacity, stride;
    keys.GetDimensions(capacity, stride);
    for (uint i = 0u; i < kCacheProbes; ++i)
    {
        const uint slot = (cell.y + i) & (capacity - 1u);
        uint previous;
        InterlockedCompareExchange(keys[slot], 0u, cell.x, previous);
        if (previous == 0u || previous == cell.x) return slot;
    }
    return ~0u;
}

// Adds one sample's radiance to a slot's frame sum.
void cacheAdd(RWStructuredBuffer<uint4> accum, uint slot, float3 radiance)
{
    const uint3 q = uint3(clamp(radiance, 0.0, kCacheMaxRadiance) * kCacheScale + 0.5);
    InterlockedAdd(accum[slot].x, q.x);
    InterlockedAdd(accum[slot].y, q.y);
    InterlockedAdd(accum[slot].z, q.z);
}

void cacheCount(RWStructuredBuffer<uint4> accum, uint slot) { InterlockedAdd(accum[slot].w, 1u); }

// The radiance a cell holds, false when it has none.
bool cacheLookup(RWStructuredBuffer<uint> keys, RWStructuredBuffer<uint4> resolved, uint2 cell, out float3 radiance)
{
    radiance = float3(0.0);
    const uint slot = cacheFind(keys, cell);
    if (slot == ~0u) return false;
    const uint4 r = resolved[slot];
    if ((r.w & 0xFFFFu) == 0u) return false;
    radiance = asfloat(r.xyz);
    return true;
}
