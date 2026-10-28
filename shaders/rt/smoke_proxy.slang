// Smoke's proxy spheres for RT shadows and RT GI (sub-project 12, step 4; spec "Smoke in RT GI and
// shadows", OPEN ruling 18). The renderer writes the frame's spheres (ohao/vfx/smoke_math.hpp, through
// rtc::AlphaTables) into a byte buffer in set 1, binding 7: the header (x the count, y 1 when the
// traces attenuate through them) at byte 0, SmokeSphereGpu i at kSmokeSpheresOffset + i * stride.
//
// smokeTransmittance is the product of exp(-density * chord) over the spheres the segment
// [0, tMax] of a unit direction crosses: 1 with no smoke, so a frame without any is unchanged bit
// for bit. The shadow trace draws its visibility from it stochastically (a sample is lit with that
// probability), the GI trace multiplies its radiance by it and adds in-scatter by 1 - T.
import shared.layout;

[[vk::binding(7, 1)]] ByteAddressBuffer smokeProxies;   // SmokeProxyHeaderGpu, then SmokeSphereGpu[]

// The flat in-scatter a GI ray through a cloud adds, weighted by 1 - T (linear radiance, a constant
// ambient of the cloud rather than a light's).
static const float kSmokeInscatter = 0.12;

// Transmittance over [0, tMax] of the ray from `o` along the unit direction `d`.
float smokeTransmittance(float3 o, float3 d, float tMax)
{
    const uint4 header = smokeProxies.Load4(0);
    if (header.y == 0u) return 1.0;
    float T = 1.0;
    const uint count = min(header.x, kSmokeSpheresMax);
    for (uint i = 0u; i < count; ++i)
    {
        const uint at = kSmokeSpheresOffset + i * kSmokeSphereStride;
        const float4 c = asfloat(smokeProxies.Load4(at));          // xyz centre, w radius
        const float density = asfloat(smokeProxies.Load(at + 16u));
        const float3 oc = o - c.xyz;
        const float b = dot(oc, d);
        const float disc = b * b - (dot(oc, oc) - c.w * c.w);
        if (disc <= 0.0) continue;
        const float sq = sqrt(disc);
        const float t0 = max(-b - sq, 0.0);
        const float t1 = min(-b + sq, tMax);
        if (t1 <= t0) continue;
        T *= exp(-density * (t1 - t0));
    }
    return T;
}
