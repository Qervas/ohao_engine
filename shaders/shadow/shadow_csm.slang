// Cascaded shadow map, depth only. The pass renders each cascade in its own render pass into its own
// array-layer view, so the vertex shader projects straight into that cascade: no geometry shader, no
// layer routing. The object's world matrix comes from RenderScene's instance table (set 0), through
// the draw record firstInstance names (GPU culling's commands); the cascade's view-projection rides
// in a 64-byte push constant (CSMPass::ShadowPushConstant).
import shared.layout;
import includes.scene.scene_data;

[[vk::binding(0, 0)]] StructuredBuffer<InstanceGpu> instances;
[[vk::binding(1, 0)]] StructuredBuffer<DrawRecord> draws;

struct ShadowPush
{
    float4x4 viewProj;
};
[[vk::push_constant]] ConstantBuffer<ShadowPush> pc;

[shader("vertex")]
float4 main([[vk::location(0)]] float3 position : POSITION, uint instance : SV_VulkanInstanceID) : SV_Position
{
    // The matrices multiply first, as GLSL's `viewProj * world * p` did.
    return mul(mul(pc.viewProj, instances[draws[instance].draw.w].world), float4(position, 1.0));
}
