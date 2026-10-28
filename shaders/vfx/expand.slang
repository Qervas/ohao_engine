// vfx.expand (sub-project 12, "The frame: expansion, sorting, the forward pass"): one thread a
// particle slot of the frame's batches (an emitter of an effect record, in record order, its
// particles in index order). Each particle is a closed form of its batch's record and its age
// (vfx/particle_math.hpp's expressions, the CPU reference): position under gravity and drag, size,
// colour and alpha over life, flipbook frame. A lit emitter's particle is lit at its centre: the
// clustered lights through the froxel it is in, the sun through one shadow ray against the scene
// (kRayMaskScene), and ambient from RT GI's radiance at its screen position, else the environment's
// irradiance, else the constant ambient (OPEN ruling 13). No atomics, no frame counter: slot i is
// the same particle every run.
import shared.layout;
import includes.lighting.clustered_lights;

[[vk::binding(0, 0)]] StructuredBuffer<VfxEmitterGpu> emitters;
[[vk::binding(1, 0)]] StructuredBuffer<VfxBatchGpu> batches;
[[vk::binding(2, 0)]] RWStructuredBuffer<VfxParticleGpu> particles;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> order;             // the draw order, identity here (vfx.sort reorders)
[[vk::binding(4, 0)]] StructuredBuffer<LightGpu> lights;
[[vk::binding(5, 0)]] StructuredBuffer<uint> clusters;
[[vk::binding(6, 0)]] RaytracingAccelerationStructure scene;      // a 1-instance stand-in without RT (flag 8 off)
[[vk::binding(7, 0)]] Texture2D<float4> giRadiance;               // RT GI: incoming radiance (rgb), full resolution
[[vk::binding(8, 0)]] Texture2D<float4> envIrradiance;            // the environment's irradiance over pi (equirect)
[[vk::binding(9, 0)]] SamplerState linearSampler;
[[vk::push_constant]] ConstantBuffer<VfxExpandPush> pc;

// vfx/particle_math.hpp's vfxHash and vfxUnit, bit for bit.
uint vfxHash(uint seed, uint index, uint attribute)
{
    uint h = seed * 0x9E3779B1u ^ (index + 0x7F4A7C15u) * 0x85EBCA77u ^ (attribute + 0x165667B1u) * 0xC2B2AE3Du;
    h = h * 747796405u + 2891336453u;
    const uint word = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    return (word >> 22u) ^ word;
}

float vfxUnit(uint seed, uint index, uint attribute)
{
    return float(vfxHash(seed, index, attribute) >> 8u) * (1.0 / 16777216.0);
}

static const uint kDrawLife = 1u, kDrawSpeed = 2u, kDrawConeZ = 3u, kDrawConePhi = 4u, kDrawFrame = 5u;

float3 colorAt(VfxEmitterGpu em, float t)
{
    const float4 keys[4] = { em.color0, em.color1, em.color2, em.color3 };
    const uint n = clamp(em.atlas.z, 1u, kVfxCurveKeys);
    if (t <= keys[0].x) return keys[0].yzw;
    for (uint i = 1u; i < n; ++i)
    {
        if (t <= keys[i].x)
        {
            const float span = keys[i].x - keys[i - 1u].x;
            const float f = span > 0.0 ? (t - keys[i - 1u].x) / span : 1.0;
            return keys[i - 1u].yzw + (keys[i].yzw - keys[i - 1u].yzw) * f;
        }
    }
    return keys[n - 1u].yzw;
}

float alphaAt(VfxEmitterGpu em, float t)
{
    const uint n = clamp(em.atlas.w, 1u, kVfxCurveKeys);
    if (t <= em.alphaT[0]) return em.alphaV[0];
    for (uint i = 1u; i < n; ++i)
    {
        if (t <= em.alphaT[i])
        {
            const float span = em.alphaT[i] - em.alphaT[i - 1u];
            const float f = span > 0.0 ? (t - em.alphaT[i - 1u]) / span : 1.0;
            return em.alphaV[i - 1u] + (em.alphaV[i] - em.alphaV[i - 1u]) * f;
        }
    }
    return em.alphaV[n - 1u];
}

float3 coneDirection(float3 axis, float spreadDeg, float u, float v)
{
    const float cosMax = cos(radians(clamp(spreadDeg, 0.0, 180.0)));
    const float z = 1.0 - u * (1.0 - cosMax);
    const float r = sqrt(max(0.0, 1.0 - z * z));
    const float phi = 6.2831853 * v;
    const float3 a = abs(axis.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    const float3 x = normalize(cross(a, axis));
    const float3 y = cross(axis, x);
    return normalize(x * (r * cos(phi)) + y * (r * sin(phi)) + axis * z);
}

float3 ballistic(float3 x0, float3 v0, float g, float k, float t)
{
    const float3 G = float3(0.0, -g, 0.0);
    if (k < 1e-6) return x0 + v0 * t + G * (0.5 * t * t);
    const float decay = 1.0 - exp(-k * t);
    return x0 + v0 * (decay / k) + G * (t / k - decay / (k * k));
}

// The batch holding slot `i`: the last whose first slot is at most i.
uint batchOf(uint i)
{
    uint lo = 0u, hi = pc.config.y;
    while (hi - lo > 1u)
    {
        const uint mid = (lo + hi) / 2u;
        if (batches[mid].info.z <= i) lo = mid;
        else hi = mid;
    }
    return lo;
}

// The sun's visibility from `p`: one ray along every directional light that casts shadows
// would cost a ray each; the first directional light with a shadow channel stands for the sun.
float sunVisibility(float3 p)
{
    if ((pc.config.w & 8u) == 0u) return 1.0;
    for (uint k = 0u; k < pc.config.z; ++k)
    {
        const LightGpu light = lights[k];
        if (int(light.position.w) != 0 || int(round(light.params.w)) < 0) continue;
        RayDesc ray;
        ray.Origin = p;
        ray.TMin = 0.01;
        ray.Direction = normalize(-light.direction.xyz);
        ray.TMax = 10000.0;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> q;
        q.TraceRayInline(scene, 0, kRayMaskScene, ray);
        q.Proceed();
        return q.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
    }
    return 1.0;
}

[shader("compute")]
[numthreads(kVfxGroupSize, 1, 1)]
void expand(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= pc.config.x) return;
    const uint b = batchOf(i);
    const VfxBatchGpu batch = batches[b];
    const VfxEmitterGpu em = emitters[batch.info.x];
    const uint index = i - batch.info.z;
    const uint seed = batch.info.y;
    const float age = batch.timing.x;

    VfxParticleGpu p;
    const float life = em.lifeSpeed.x + (em.lifeSpeed.y - em.lifeSpeed.x) * vfxUnit(seed, index, kDrawLife);
    const bool alive = age >= 0.0 && age < life;
    // The record's -Z and origin (glm's columns 2 and 3; m[i] in Slang is a row).
    const float3 axis = normalize(mul(batch.pose, float4(0.0, 0.0, -1.0, 0.0)).xyz);
    const float3 dir = coneDirection(axis, em.shape.z, vfxUnit(seed, index, kDrawConeZ), vfxUnit(seed, index, kDrawConePhi));
    const float speed = em.lifeSpeed.z + (em.lifeSpeed.w - em.lifeSpeed.z) * vfxUnit(seed, index, kDrawSpeed);
    const float3 x0 = mul(batch.pose, float4(0.0, 0.0, 0.0, 1.0)).xyz + axis * em.motion.y;
    const float t = max(age, 0.0);
    const float3 position = ballistic(x0, dir * speed, em.shape.w, em.motion.x, t);
    const float f = life > 0.0 ? clamp(t / life, 0.0, 1.0) : 1.0;
    const float size = em.shape.x + (em.shape.y - em.shape.x) * f;
    float3 color = colorAt(em, f) * em.motion.z;
    const float alpha = alphaAt(em, f);
    const uint frames = max(em.atlas.x * em.atlas.y, 1u);
    const uint mode = em.look.w & 0xFFu;
    uint frame = 0u;
    if (mode == 0u) frame = min(frames - 1u, uint(f * float(frames)));
    else if (mode == 1u) frame = vfxHash(seed, index, kDrawFrame) % frames;
    else frame = min(em.look.w >> 8u, frames - 1u);

    float3 light = float3(0.0);
    if (alive && em.look.y != 0u)
    {
        // The froxel the particle's centre is in, and its pixel for RT GI's radiance.
        const float4 clip = mul(pc.viewProj, float4(position, 1.0));
        const float2 ndc = clip.w > 0.0 ? clip.xy / clip.w : float2(0.0);
        const float2 pixel = (ndc * 0.5 + 0.5) * pc.screen.xy;
        const float viewDepth = -(dot(pc.viewZ.xyz, position) + pc.viewZ.w);
        // A froxel holds the lights of its own frustum slice only: a particle off screen (or
        // behind the camera) sums every light instead.
        const bool onScreen = clip.w > 0.0 && all(abs(ndc) <= 1.0);
        const bool clustered = (pc.config.w & 1u) != 0u && onScreen;
        const uint2 list = lightList(clusters, clustered, pc.config.z, clamp(pixel, float2(0.0), pc.screen.xy - 1.0),
                                     pc.screen.xy, viewDepth, pc.screen.z, pc.screen.w);
        light = particleLight(position, lights, clusters, list, clustered, sunVisibility(position));
        float3 ambient;
        if ((pc.config.w & 2u) != 0u && clip.w > 0.0 && all(abs(ndc) <= 1.0))
            ambient = giRadiance.SampleLevel(linearSampler, ndc * 0.5 + 0.5, 0.0).rgb;
        else if ((pc.config.w & 4u) != 0u)
            ambient = pc.camera.w * envIrradiance.SampleLevel(linearSampler, equirectUV(float3(0.0, 1.0, 0.0)), 0.0).rgb;
        else
            ambient = float3(pc.camera.w);
        light += ambient;
        color *= light;
    }

    p.posSize = float4(position, alive ? size : 0.0);
    p.color = float4(color, alive ? alpha : 0.0);
    const float dist = distance(position, pc.camera.xyz);
    p.info = uint4(frame, b, asuint(dist), alive ? 1u : 0u);
    p.light = float4(light, 0.0);
    particles[i] = p;
    order[i] = i;
}
