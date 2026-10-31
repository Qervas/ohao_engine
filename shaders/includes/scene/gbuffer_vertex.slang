// The G-buffer's vertex transform, one function for every path that draws into it: the instance
// path's vertex shader (core/gbuffer.slang, also the transparent pass's meshes) and the meshlet
// path's mesh and pull shaders (cull/meshlets.slang). One function, so their images are identical
// (cull_parity_gpu_test, test 7). Its outputs are core/gbuffer.slang's fragment inputs.
module gbuffer_vertex;

public struct GVertex
{
    public float4 position : SV_Position;
    [[vk::location(0)]] public float3 worldPos : WORLDPOS;
    [[vk::location(1)]] public float3 normal : NORMAL;
    [[vk::location(2)]] public float3 color : COLOR;
    [[vk::location(3)]] public float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] public float2 texCoord1 : TEXCOORD1;
    [[vk::location(5)]] public float4 tangent : TANGENT;
    [[vk::location(6)]] public float4 currentPos : CURRENTPOS;
    [[vk::location(7)]] public float4 prevPos : PREVPOS;
    [[vk::location(8)]] public nointerpolation uint instance : INSTANCE;
    [[vk::location(9)]] public nointerpolation uint materialSlot : MATERIALSLOT;
};

// An instance's matrices, the same for all of a draw's vertices (a mesh workgroup makes them once).
public struct InstanceXform
{
    public float4x4 world;
    public float4x4 prevWorld;
    public float3x3 normalMatrix;   // the world matrix's upper 3x3, inverse transposed
};

public InstanceXform instanceXform(float4x4 world, float4x4 prevWorld)
{
    InstanceXform x;
    x.world = world;
    x.prevWorld = prevWorld;
    const float3x3 w3 = float3x3(world[0].xyz, world[1].xyz, world[2].xyz);
    // Slang has no inverse(): GLSL.std.450's MatrixInverse, as glslc emitted it.
    const float3x3 inv = spirv_asm { result:$$float3x3 = OpExtInst glsl450 MatrixInverse $w3 };
    x.normalMatrix = transpose(inv);
    return x;
}

// A vertex (object space: p, n, t; prevLocal its last-frame position, a skinned copy's
// boneWeights.xyz, else p) to the G-buffer's interpolants: the world position, the normal and
// tangent through the inverse transpose, this and last frame's clip positions.
public GVertex transformVertex(InstanceXform x, float4x4 viewProj, float4x4 prevViewProj, float3 p, float3 color,
                               float3 n, float4 t, float2 uv0, float2 uv1, float3 prevLocal, uint instance,
                               uint materialSlot)
{
    GVertex v;
    const float4 worldPos = mul(x.world, float4(p, 1.0));
    v.worldPos = worldPos.xyz;
    v.normal = normalize(mul(x.normalMatrix, n));
    v.tangent = float4(normalize(mul(x.normalMatrix, t.xyz)), t.w);
    v.color = color;
    v.texCoord = uv0;
    v.texCoord1 = uv1;
    v.currentPos = mul(viewProj, worldPos);
    v.prevPos = mul(prevViewProj, mul(x.prevWorld, float4(prevLocal, 1.0)));
    v.instance = instance;
    v.materialSlot = materialSlot;
    v.position = v.currentPos;
    return v;
}
