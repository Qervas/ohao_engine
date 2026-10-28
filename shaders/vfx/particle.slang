// The transparent pass's particles (sub-project 12): one batch a draw, six vertices a particle, in
// the batch's draw order (vfx.sort's for a sorted alpha batch, else slot order). Camera-facing quads
// through the jittered view-projection the G-buffer used; a flipbook frame from the bindless table
// (or a soft round dot), times the particle's lit colour; a soft fade against the scene's depth
// over the emitter's `soft` metres. Colour goes to the lit HDR image (the pipeline's blend state is
// the emitter's: additive, alpha or premultiplied), coverage to the transparency mask (R8, MAX)
// that TAA lowers its history weight under.
import shared.layout;

[[vk::binding(0, 0)]] StructuredBuffer<VfxParticleGpu> particles;
[[vk::binding(1, 0)]] StructuredBuffer<uint> order;
[[vk::binding(2, 0)]] StructuredBuffer<VfxEmitterGpu> emitters;
[[vk::binding(3, 0)]] Texture2D<float> sceneDepth;   // the G-buffer's depth (read-only, also the depth attachment)
[[vk::binding(0, 1)]] Sampler2D textures[4096];      // the bindless table (sized: see transparent_mesh.slang)
[[vk::push_constant]] ConstantBuffer<VfxDrawPush> pc;

struct VsOut
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
    [[vk::location(1)]] float4 color;
    [[vk::location(2)]] nointerpolation uint frame;
    [[vk::location(3)]] float viewDepth;
};

static const float2 kCorners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };

[shader("vertex")]
VsOut vertexMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const uint slot = pc.range.z != 0u ? order[pc.range.x + instanceId] : pc.range.x + instanceId;
    const VfxParticleGpu p = particles[slot];
    const float2 c = kCorners[vertexId];
    const float halfSize = 0.5 * p.posSize.w;
    const float3 world = p.posSize.xyz + (pc.right.xyz * c.x + pc.up.xyz * c.y) * halfSize;
    VsOut o;
    o.position = mul(pc.viewProj, float4(world, 1.0));
    o.uv = c * 0.5 + 0.5;
    o.color = p.color;
    o.frame = p.info.x;
    o.viewDepth = o.position.w;   // the perspective w is the view depth
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
    const VfxEmitterGpu em = emitters[pc.range.y];
    float4 tex;
    if (em.look.z == kVfxNoTexture)
    {
        const float r = length(i.uv * 2.0 - 1.0);
        tex = float4(1.0, 1.0, 1.0, saturate(1.0 - r * r));
    }
    else
    {
        const uint cols = max(em.atlas.x, 1u), rows = max(em.atlas.y, 1u);
        const float2 cell = float2(float(i.frame % cols), float(i.frame / cols));
        const float2 uv = (cell + i.uv) / float2(float(cols), float(rows));
        tex = textures[NonUniformResourceIndex(em.look.z)].SampleLevel(uv, 0.0);
    }
    float alpha = i.color.a * tex.a;
    // Soft: fade where the scene is within `soft` metres behind the particle.
    if (em.motion.w > 0.0)
    {
        const float d = sceneDepth.Load(int3(int2(i.position.xy), 0));
        const float sceneView = pc.up.w / (d + pc.right.w);
        alpha *= saturate((sceneView - i.viewDepth) / em.motion.w);
    }
    PsOut o;
    const float3 rgb = i.color.rgb * tex.rgb;
    o.color = float4(em.look.x == kVfxBlendPremultiplied ? rgb * alpha : rgb, alpha);
    o.coverage = alpha;
    return o;
}
