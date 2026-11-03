// Path guiding's learning (sub-project 23; spec "Learning" and "Deterministic commit"): after the
// path tracer's frame, its proposals become the grid's next states, Algorithm 3 of Alber, Hanika,
// Dachsbacher (2025) with the sample's light from the path itself, committed so that one write per
// slot wins by a hash, the same in every run. Three passes over the proposals:
//   update  the sample's luminance, the acceptance, the maximum-likelihood step or the discard
//           rule; a pending write and its key, InterlockedMax of the key on the target slot;
//   owner   among the writes whose key won a slot, the lowest proposal index (a tie of keys), as
//           the largest ~index, so the host clears the claims to 0 with one fill;
//   write   that proposal writes its state.
// The host copies the written buffer over the read one after the write pass.

import shared.layout;
import sampling.vmf;
import rt.path_guiding;

[[vk::binding(0, 0)]] StructuredBuffer<GuideState> guideStates;   // what the frame read
[[vk::binding(1, 0)]] RWStructuredBuffer<GuideState> guideWrite;  // the next frame's
[[vk::binding(2, 0)]] RWStructuredBuffer<uint2> guideClaim;       // per slot: x the winning key, y ~its owner (0: none)
[[vk::binding(3, 0)]] RWStructuredBuffer<GuideProposal> guideProposals;
[[vk::binding(4, 0)]] StructuredBuffer<float4> guidePixel;        // per pixel: the sample's radiance, proposals written

[[vk::push_constant]] ConstantBuffer<GuidePush> pc;

static const float kGuideDiscard = 1.0 / 16.0;   // a state whose lobe now finds this much less is dropped

float guideLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// The proposal's word 0-1 and 8-15 once a write is pending.
void setPending(inout GuideProposal p, uint slot, uint key, GuideState s)
{
    p.x.x = asfloat(slot);
    p.x.y = asfloat(key);
    p.rk = float3(s.w, s.y.x, s.y.y);
    p.meanW = s.y.z;
    p.beta = float3(s.r, asfloat(s.nt), asfloat(s.check));
    p.flags |= kGuidePending;
}

GuideState pendingState(GuideProposal p)
{
    GuideState s = guideEmpty();
    s.w = p.rk.x;
    s.y = float3(p.rk.y, p.rk.z, p.meanW);
    s.r = p.beta.x;
    s.nt = asuint(p.beta.y);
    s.check = asuint(p.beta.z);
    return s;
}

[shader("compute")]
[numthreads(kGuideGroupSize, 1, 1)]
void update(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= pc.config.y) return;
    GuideProposal p = guideProposals[i];
    p.flags &= ~kGuidePending;
    const float4 pixel = guidePixel[i / kGuideTrainVertices];
    const bool written = (i % kGuideTrainVertices) < asuint(pixel.w);
    if (!written || (p.flags & kGuideHasNext) == 0u)
    {
        guideProposals[i].flags = p.flags & ~kGuidePending;
        return;
    }
    // The sample's luminance: f cos L_i / q along the drawn direction, the path's light after this
    // vertex over the throughput that reached it (per channel; a channel the path lost gives 0).
    const float3 dr = max(pixel.xyz - p.rk, float3(0.0));
    const float3 c = float3(p.beta.x > 0.0 ? dr.x / p.beta.x : 0.0, p.beta.y > 0.0 ? dr.y / p.beta.y : 0.0,
                            p.beta.z > 0.0 ? dr.z / p.beta.z : 0.0);
    const float f = guideLuminance(c);
    const uint frame = pc.config.x;
    uint h = guidePcg(i ^ guidePcg(frame ^ 0x3C6EF372u));
    const float3 n = guideOctUnpack(p.normal);
    GuideState s = guideEmpty();
    if (p.srcSlot != kGuideNoSlot) s = guideStates[p.srcSlot];
    bool write = false;
    uint slot = 0u;
    GuideState o = s;
    // The acceptance (Eq. 8): against the read states' mean weight (always, when none was read).
    if (f > 0.0 && guideUnit(h) * p.meanW < f)
    {
        const uint3 cell = guideCell(p.x, n, pc.camera.xyz, h);   // a fresh place, as a read picks one
        o = guideUpdate(s, p.x, p.xnext, f, frame);
        o.check = cell.y;
        slot = cell.x;
        // The slot's occupant stays unless the new state outweighs it by a coin (w against w): a
        // population with memory. Replacing whatever was there (the paper's last writer) let every
        // frame's many dim samples overwrite the few states that had found a small bright source
        // (tools/guiding/mcpg_sim.py: 1 run in 8 learned a ceiling patch; with this, 8 in 8).
        const GuideState occupant = guideStates[slot];
        write = !(occupant.w > 0.0) || guideUnit(h) * occupant.w < o.w;
    }
    else if (p.srcSlot != kGuideNoSlot && s.w > 0.0)
    {
        // The discard rule (section 5): the sample went where the state's light was and found far
        // less, so that light has gone; its state stops drawing samples.
        float3 mu;
        float kappa;
        if (guideLobe(s, p.x, asfloat(pc.config.w), mu, kappa))
        {
            const float rbar = vmfMeanCosine(kappa);
            if (dot(normalize(p.xnext - p.x), mu) > rbar && f < kGuideDiscard * s.w)
            {
                o.w = 0.0;
                slot = p.srcSlot;
                write = true;
            }
        }
    }
    if (write)
    {
        // The slot's winner among this frame's writes by weight (Efraimidis and Spirakis' key
        // u^(1/w), the largest wins: a write wins with probability by its weight), as float bits
        // (positive floats order as their bits). A discard (w 0) takes the smallest key.
        const float u = max(guideUnit(h), 1e-30);
        const uint key = max(asuint(o.w > 0.0 ? pow(u, 1.0 / o.w) : 0.0), 1u);
        setPending(p, slot, key, o);
        InterlockedMax(guideClaim[slot].x, key);
    }
    guideProposals[i] = p;
}

[shader("compute")]
[numthreads(kGuideGroupSize, 1, 1)]
void owner(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= pc.config.y) return;
    const GuideProposal p = guideProposals[i];
    if ((p.flags & kGuidePending) == 0u) return;
    const uint slot = asuint(p.x.x);
    if (guideClaim[slot].x == asuint(p.x.y)) InterlockedMax(guideClaim[slot].y, ~i);
}

[shader("compute")]
[numthreads(kGuideGroupSize, 1, 1)]
void write(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= pc.config.y) return;
    const GuideProposal p = guideProposals[i];
    if ((p.flags & kGuidePending) == 0u) return;
    const uint slot = asuint(p.x.x);
    if (guideClaim[slot].y == ~i) guideWrite[slot] = pendingState(p);
}
