// capture.preview (sub-project 2b spec, "Capture budgets"): an inline capture's preview, made
// on the GPU in the capture frame. A box (area) filter of a rectangle of the final RGBA8 image
// down to the preview's size, written packed into the frame's host-cached staging buffer, so
// the CPU reads the preview and nothing else. The filter is runtime::fitWithin's: each output
// pixel averages the source pixels its footprint covers, weighted by the area covered, the
// vertical sum taken first; the bytes match the CPU's within 1 (float rounding).
import shared.layout;

[[vk::binding(0, 0)]] Texture2D<float4> finalImage;     // RGBA8 UNORM, shader read-only
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> preview;  // dst.x * dst.y texels, RGBA8 packed, rows top down
[[vk::push_constant]] ConstantBuffer<PreviewPush> pc;

// Output index i's footprint in a source of `src` pixels scaled to `dst`: [a, b), its first
// pixel and one past its last.
struct Footprint
{
    float a, b, scale;
    uint lo, hi;
};

Footprint footprint(uint i, uint src, uint dst)
{
    Footprint f;
    f.scale = float(src) / float(dst);
    f.a = float(i * src) / float(dst);         // exact where i * src / dst is an integer
    f.b = float((i + 1u) * src) / float(dst);
    f.lo = uint(floor(f.a));
    f.hi = min(src, uint(ceil(f.b)));
    return f;
}

float cover(Footprint f, uint k)
{
    return (min(f.b, float(k + 1u)) - max(f.a, float(k))) / f.scale;
}

// One row of 64: a warp's writes are 128 contiguous bytes of host memory.
[shader("compute")]
[numthreads(64, 1, 1)]
void downscale(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.dst.xy)) return;
    const Footprint fx = footprint(id.x, pc.src.z, pc.dst.x);
    const Footprint fy = footprint(id.y, pc.src.w, pc.dst.y);
    float4 sum = float4(0.0);
    for (uint x = fx.lo; x < fx.hi; ++x)
    {
        float4 column = float4(0.0);
        for (uint y = fy.lo; y < fy.hi; ++y)
        {
            // UNORM back to its byte exactly, as the CPU filter reads bytes.
            const float4 texel = round(finalImage.Load(int3(int2(pc.src.xy + uint2(x, y)), 0)) * 255.0);
            column += cover(fy, y) * texel;
        }
        sum += cover(fx, x) * column;
    }
    const uint4 c = uint4(min(sum + 0.5, float4(255.0)));
    preview[id.y * pc.dst.x + id.x] = c.r | (c.g << 8u) | (c.b << 16u) | (c.a << 24u);
}
