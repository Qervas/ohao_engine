// Path guiding's grid (sub-project 23; docs/specs/2026-11-02-path-guiding-design.md, "The grid"
// and "A bounce"): Alber, Hanika, Dachsbacher (2025)'s Markov chain states in world-space hash
// grids, read at a path vertex into a mixture of vMF lobes whose exact density the path tracer
// weighs its bounce by. Our code from the paper's text; its authors' implementation is not used.
//
// Two grids share one buffer of kGuideCapacity states (sampling/vmf.slang's GuideState):
//   adaptive  the radiance cache's voxels (a level per doubling of distance from the camera, 5 cm
//             within 1 m), keyed by the voxel, the level and the side the surface faces;
//   static    1 m voxels keyed by the voxel alone (a state is used only if its lobe is above the
//             surface), so light found on one side of an edge reaches the other.
// A read takes a grid vertex near the point with probability by its trilinear weight (the paper's
// stochastic interpolation) and the adaptive grid at a level coarser than the point's with
// probability halving per level, so states spread between neighbouring cells and levels.
//
// The path tracer reads the buffer frame N-1 left (binding 46) and writes proposals (49) and the
// pixel's final radiance (50); rt/path_guiding_update.slang turns them into frame N's states.

import shared.layout;
import sampling.vmf;
import rt.radiance_cache;

static const uint kGuideCandidates = 5u;       // states read at a vertex (the paper's N_mc)
static const float kGuideAdaptive = 0.7;       // a read's chance of the adaptive grid (the paper's)
static const float kGuideStaticVoxel = 1.0;    // the static grid's voxel (m)
static const float kGuideFar = 1.0e4;          // an escaped sample's point: this far along its direction
static const uint kGuideNoSlot = 0xFFFFFFFFu;

// Proposal flags (word 7).
static const uint kGuideHasNext = 1u;          // the sample's ray was traced: xnext is its end
static const uint kGuidePending = 2u;          // the update pass decided a write (target in word 0)

// A pixel's proposal for one guided vertex (kGuideProposalWords words). After the update pass,
// words 0-1 hold the write's target slot and key, words 8-15 the state to write.
struct GuideProposal
{
    float3 x;       // the vertex
    uint normal;    // its geometric normal, octahedral 16:16
    float3 xnext;   // where the drawn direction's ray ended (kGuideFar along it when it escaped)
    uint flags;
    float3 rk;      // the path's radiance after this vertex's light sampling
    float meanW;    // the read states' mean weight (the acceptance's reference)
    float3 beta;    // the throughput arriving at the vertex
    uint srcSlot;   // the state the bounce was drawn from (kGuideNoSlot: the BSDF)
};

uint guidePcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float guideUnit(inout uint h)
{
    h = guidePcg(h);
    return float(h >> 8u) * (1.0 / 16777216.0);
}

uint guideOctPack(float3 n)
{
    float2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) p = (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    const uint2 q = uint2(clamp(p * 0.5 + 0.5, 0.0, 1.0) * 65535.0 + 0.5);
    return q.x | (q.y << 16u);
}

float3 guideOctUnpack(uint v)
{
    const float2 p = float2(float(v & 0xFFFFu), float(v >> 16u)) / 65535.0 * 2.0 - 1.0;
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// A grid cell for a read or a write at x (geometric normal n): which grid, which level, which
// vertex, all drawn from h. x: the slot; y: the check word (never 0); z: 1 for the static grid.
uint3 guideCell(float3 x, float3 n, float3 camera, inout uint h)
{
    const bool adaptive = guideUnit(h) < kGuideAdaptive;
    float voxel;
    uint tag;
    if (adaptive)
    {
        h = guidePcg(h);
        const uint coarser = min(firstbitlow(h | 0x80000000u), kCacheMaxLevel);   // P(k) = 2^-(k+1)
        const uint level = min(cacheLevel(length(x - camera)) + coarser, kCacheMaxLevel);
        voxel = cacheVoxelSize(level);
        tag = 1u + level * 8u + cacheSide(n);
    }
    else
    {
        voxel = kGuideStaticVoxel;
        tag = 0x5A17u;
    }
    // A vertex of the voxel around x, each axis by its trilinear weight.
    const float3 g = x / voxel;
    const float3 f = g - floor(g);
    int3 v = int3(floor(g));
    if (guideUnit(h) < f.x) v.x += 1;
    if (guideUnit(h) < f.y) v.y += 1;
    if (guideUnit(h) < f.z) v.z += 1;
    uint k = cacheHash(uint(v.x) ^ 0x2545F491u);
    k = cacheHash(k ^ uint(v.y));
    k = cacheHash(k ^ uint(v.z));
    k = cacheHash(k ^ tag);
    return uint3(k & (kGuideCapacity - 1u), cacheHash(k ^ 0x68E31DA4u) | 1u, adaptive ? 0u : 1u);
}

// The lobes read at a vertex and their mixture (spec "A bounce", steps 1-2).
struct GuideMixture
{
    float3 mu[kGuideCandidates];
    float kappa[kGuideCandidates];
    float w[kGuideCandidates];        // each state's weight (0: empty, collided or below the surface)
    uint slot[kGuideCandidates];
    float total;                      // the sum of the weights
    float g;                          // the guided share: kGuideProb when total > 0, else 0
};

GuideMixture guideRead(StructuredBuffer<GuideState> grid, float3 x, float3 n, float3 camera, uint seed, float prior,
                       float share)
{
    GuideMixture m;
    m.total = 0.0;
    uint h = seed;
    [unroll] for (uint i = 0u; i < kGuideCandidates; ++i)
    {
        const uint3 c = guideCell(x, n, camera, h);
        const GuideState s = grid[c.x];
        float3 mu;
        float kappa;
        const bool ok = s.check == c.y && guideLobe(s, x, prior, mu, kappa) && dot(mu, n) > 0.0;
        m.mu[i] = mu;
        m.kappa[i] = kappa;
        m.w[i] = ok ? s.w : 0.0;
        m.slot[i] = c.x;
        m.total += m.w[i];
    }
    m.g = m.total > 0.0 ? share : 0.0;
    return m;
}

// The bounce's density at a world direction, given the BSDF's there: the one-sample MIS mixture
// (balance heuristic) of the BSDF and every read lobe by its resampling probability. Exact for the
// states read, whatever they hold.
float guidePdf(GuideMixture m, float3 dir, float bsdfPdf)
{
    if (!(m.g > 0.0)) return bsdfPdf;
    float lobes = 0.0;
    [unroll] for (uint i = 0u; i < kGuideCandidates; ++i)
        if (m.w[i] > 0.0) lobes += m.w[i] * vmfPdf(dir, m.mu[i], m.kappa[i]);
    return (1.0 - m.g) * bsdfPdf + m.g * lobes / m.total;
}

// The candidate a uniform u picks, by weight (the paper's weighted reservoir).
uint guidePick(GuideMixture m, float u)
{
    float target = u * m.total;
    uint pick = 0u;
    [unroll] for (uint i = 0u; i < kGuideCandidates; ++i)
    {
        if (m.w[i] > 0.0)
        {
            pick = i;
            if (target < m.w[i]) break;
            target -= m.w[i];
        }
    }
    return pick;
}
