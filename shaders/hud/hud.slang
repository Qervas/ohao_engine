// The HUD (sub-project 7 spec, "Presentation"): render/hud/hud_pass.cpp draws the frame's
// HudList over the final RGBA8 image, one instance (six vertices) an item, alpha blended in list
// order. Items are pixel rectangles (origin top left, y down): solid, a glyph of the 5x7 font
// (render/hud/hud_list.cpp, uploaded once: row r of glyph g is font[g * 7 + r], bit 4 the leftmost
// column), or a vignette whose alpha grows from 0 inside to color.a at the corners.

struct HudItem
{
    float4 rect;    // x0, y0, x1, y1 in pixels
    float4 color;   // rgba
    uint kind;      // 0 rect, 1 glyph, 2 vignette
    uint glyph;
    uint pad0;
    uint pad1;
};

[[vk::binding(0, 0)]] StructuredBuffer<HudItem> items;
[[vk::binding(1, 0)]] StructuredBuffer<uint> font;

struct HudPush
{
    float2 invSize;   // 1 / the image's size in pixels
    float2 pad;
};
[[vk::push_constant]] ConstantBuffer<HudPush> pc;

struct HudVertex
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] nointerpolation uint item : ITEM;
};

static const float2 kCorners[6] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
                                    float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0) };

[shader("vertex")]
HudVertex vs(uint vertexIndex : SV_VulkanVertexID, uint instanceIndex : SV_VulkanInstanceID)
{
    const HudItem it = items[instanceIndex];
    const float2 corner = kCorners[vertexIndex];
    const float2 p = lerp(it.rect.xy, it.rect.zw, corner);
    HudVertex o;
    o.position = float4(p * pc.invSize * 2.0 - 1.0, 0.0, 1.0);
    o.uv = corner;
    o.item = instanceIndex;
    return o;
}

[shader("fragment")]
float4 fs(HudVertex v) : SV_Target
{
    const HudItem it = items[v.item];
    float4 c = it.color;
    if (it.kind == 1u)
    {
        const uint col = min(uint(v.uv.x * 5.0), 4u);
        const uint row = min(uint(v.uv.y * 7.0), 6u);
        if (((font[it.glyph * 7u + row] >> (4u - col)) & 1u) == 0u)
            discard;
    }
    else if (it.kind == 2u)
    {
        const float r = length((v.uv - 0.5) * 2.0) * 0.70710678;
        c.a *= smoothstep(0.45, 1.0, r);
    }
    return c;
}
