// The emissive lights' power image, one level up (frontier F3, render/rt/emissive_lights.hpp): each
// texel the sum of its four children, so the top texel is the total and a draw can descend the
// levels choosing children in proportion to their power (RTXDI's local light PDF map).

[[vk::binding(0, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> below;   // level k
[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> above;   // level k + 1

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    above.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const uint2 c = id.xy * 2u;
    above[id.xy] = below[c] + below[c + uint2(1u, 0u)] + below[c + uint2(0u, 1u)] + below[c + uint2(1u, 1u)];
}
