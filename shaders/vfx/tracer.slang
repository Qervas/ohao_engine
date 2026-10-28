// The transparent pass's tracers (sub-project 12, step 3): six vertices a tracer, built on the host
// from the round's ends and the record's age (a camera-facing stretched quad: its head at speed * age,
// clamped at the hit, its tail `length` behind it). Additive HDR light through the jittered
// view-projection the G-buffer used; dim at the tail, bright at the head, across its width. Coverage
// goes to the transparency mask (R8, MAX) like a particle's.
import shared.layout;

[[vk::push_constant]] ConstantBuffer<TracerPush> pc;

struct VsIn
{
    [[vk::location(0)]] float4 pos;
    [[vk::location(1)]] float4 uv;
    [[vk::location(2)]] float4 color;
};

struct VsOut
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
    [[vk::location(1)]] float4 color;
};

[shader("vertex")]
VsOut vertexMain(VsIn i)
{
    VsOut o;
    o.position = mul(pc.viewProj, float4(i.pos.xyz, 1.0));
    o.uv = i.uv.xy;
    o.color = i.color;
    return o;
}

struct PsOut
{
    [[vk::location(0)]] float4 color;
    [[vk::location(1)]] float coverage;
};

[shader("fragment")]
PsOut fragmentMain(VsOut i)
{
    const float across = 1.0 - abs(i.uv.x);
    const float along = smoothstep(0.0, 1.0, i.uv.y);
    const float a = across * across * along;
    PsOut o;
    o.color = float4(i.color.rgb * a, 0.0);
    o.coverage = a;
    return o;
}
