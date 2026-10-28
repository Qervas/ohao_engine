// vfx.sort (sub-project 12, OPEN ruling 11): the particles of one alpha-blended batch (at most
// kVfxSortMax) back to front, in one workgroup: a bitonic sort in shared memory by (view distance's
// float bits descending, particle slot ascending): integer keys, so equal distances keep slot order
// and two runs give the same order. It writes the batch's range of the draw order (`order`, which
// vfx.expand set to the identity). Additive batches are order-independent and never sorted; the
// objects themselves (batches, blended meshes) are ordered on the host.
import shared.layout;

[[vk::binding(0, 0)]] StructuredBuffer<VfxParticleGpu> particles;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> order;
[[vk::push_constant]] ConstantBuffer<VfxSortPush> pc;

groupshared uint2 gKeys[kVfxSortMax];   // x distance bits, y slot

// a before b: farther first, then the lower slot.
bool before(uint2 a, uint2 b)
{
    return a.x > b.x || (a.x == b.x && a.y < b.y);
}

[shader("compute")]
[numthreads(kVfxSortMax, 1, 1)]
void sort(uint3 tid : SV_GroupThreadID)
{
    const uint t = tid.x;
    const uint first = pc.range.x;
    const uint count = pc.range.y;
    // Padding sorts last: distance 0, the largest slot.
    gKeys[t] = t < count ? uint2(particles[first + t].info.z, first + t) : uint2(0u, 0xFFFFFFFFu);
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 2u; k <= kVfxSortMax; k <<= 1u)
    {
        for (uint j = k >> 1u; j > 0u; j >>= 1u)
        {
            const uint partner = t ^ j;
            if (partner > t)
            {
                const uint2 a = gKeys[t];
                const uint2 b = gKeys[partner];
                const bool ascending = (t & k) == 0u;   // this half in `before` order
                if (ascending ? before(b, a) : before(a, b))
                {
                    gKeys[t] = b;
                    gKeys[partner] = a;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (t < count) order[first + t] = gKeys[t].y;
}
