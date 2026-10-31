// The transform gizmo: lines and handles in their vertex colour, a highlighted axis mixed toward
// the highlight colour (GizmoPass::GizmoPushConstants).

struct GizmoParams
{
    float4x4 viewProj;
    float4x4 model;
    float4 highlightColor;   // xyz the highlight colour, w its weight (0 or 1)
};
[[vk::push_constant]] ConstantBuffer<GizmoParams> params;

struct GizmoVertex
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color : COLOR;
};

[shader("vertex")]
GizmoVertex vertexMain([[vk::location(0)]] float3 position : POSITION, [[vk::location(1)]] float3 color : COLOR)
{
    GizmoVertex o;
    o.position = mul(mul(params.viewProj, params.model), float4(position, 1.0));
    o.color = lerp(color, params.highlightColor.xyz, params.highlightColor.w);
    return o;
}

[shader("fragment")]
float4 fragmentMain([[vk::location(0)]] float3 color : COLOR) : SV_Target0
{
    return float4(color, 1.0);
}
