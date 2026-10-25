// The Hi-Z pyramid (sub-project 2b spec, "Two-pass occlusion"): the maximum depth over each
// texel's footprint, built from the early G-buffer pass's depth for the late cull. Level 0 is
// the depth buffer halved (rounded up) and padded to powers of two (GpuCulling::resize), each
// level the last halved, to 1x1; texel t of a level is the maximum of texels 2t and 2t + 1 of
// the level below (the depth's reads clamped to its edge), so level L texel t covers exactly
// pixels [t * 2^(L+1), (t + 1) * 2^(L+1)) of the frame.
import shared.layout;

[[vk::binding(0, 0)]] Texture2D<float> depth;
[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> dst;
[[vk::binding(2, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> src;
[[vk::push_constant]] ConstantBuffer<HiZPush> pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void fromDepth(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size.xy)) return;
    const int2 s = int2(id.xy * 2u);
    const int2 last = int2(pc.size.zw) - 1;
    const float a = depth.Load(int3(min(s, last), 0));
    const float b = depth.Load(int3(min(s + int2(1, 0), last), 0));
    const float c = depth.Load(int3(min(s + int2(0, 1), last), 0));
    const float d = depth.Load(int3(min(s + int2(1, 1), last), 0));
    dst[id.xy] = max(max(a, b), max(c, d));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void downsample(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.size.xy)) return;
    const int2 s = int2(id.xy * 2u);
    const int2 last = int2(pc.size.zw) - 1;
    const float a = src[uint2(min(s, last))];
    const float b = src[uint2(min(s + int2(1, 0), last))];
    const float c = src[uint2(min(s + int2(0, 1), last))];
    const float d = src[uint2(min(s + int2(1, 1), last))];
    dst[id.xy] = max(max(a, b), max(c, d));
}
