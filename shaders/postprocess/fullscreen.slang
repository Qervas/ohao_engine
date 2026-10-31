// The fullscreen triangle: three vertices from the vertex index, no vertex input, covering the
// screen ((-1, -1), (3, -1), (-1, 3)), its uv 0 to 1 across the screen.

struct FullscreenOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
FullscreenOut main(uint vertexId : SV_VulkanVertexID)
{
    const float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    const float2 texCoords[3] = { float2(0.0, 0.0), float2(2.0, 0.0), float2(0.0, 2.0) };
    FullscreenOut o;
    o.position = float4(positions[vertexId], 0.0, 1.0);
    o.uv = texCoords[vertexId];
    return o;
}
