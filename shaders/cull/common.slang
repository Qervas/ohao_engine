// GPU culling's tests, one module for every culler (sub-project 2b spec, "GPU-driven geometry",
// "Meshlets and mesh shaders"): the draws' (cull/instances.slang classify), and the meshlets'
// in the task shader (cull/meshlets.slang) and in the mesh-shader fallback's compute pass
// (cull/instances.slang classifyMeshlets), so both meshlet paths keep and drop the same
// meshlets.
import shared.layout;

// A box's footprint in clip space.
struct Bounds
{
    bool inFrustum;   // not wholly outside one clip plane
    bool nearCross;   // a corner in front of the near plane: no screen rectangle
    float minDepth;   // the nearest corner's depth
    float2 lo, hi;    // the corners' NDC rectangle
};

// The box [lo, hi] (local space) through the world matrix (columns c0..c3) and viewProj. A
// cascade clamps depth (CSM's depthClampEnable): casters before its near or past its far
// plane still render, so for it (`sidesOnly`) only the sides cull.
Bounds boxBounds(float4x4 viewProj, float4 c0, float4 c1, float4 c2, float4 c3, float3 lo, float3 hi, bool sidesOnly)
{
    Bounds b;
    b.nearCross = false;
    b.minDepth = 1.0;
    b.lo = float2(1e30);
    b.hi = float2(-1e30);
    uint allOut = 63u;
    for (uint k = 0u; k < 8u; ++k)
    {
        const float3 p = float3((k & 1u) != 0u ? hi.x : lo.x, (k & 2u) != 0u ? hi.y : lo.y, (k & 4u) != 0u ? hi.z : lo.z);
        const float4 world = c0 * p.x + c1 * p.y + c2 * p.z + c3;
        const float4 clip = mul(viewProj, world);
        uint outside = 0u;
        if (clip.x < -clip.w) outside |= 1u;
        if (clip.x > clip.w) outside |= 2u;
        if (clip.y < -clip.w) outside |= 4u;
        if (clip.y > clip.w) outside |= 8u;
        if (clip.z < 0.0) outside |= 16u;
        if (clip.z > clip.w) outside |= 32u;
        allOut &= outside;
        if (clip.w <= 1e-6 || clip.z < 0.0)
        {
            b.nearCross = true;
        }
        else
        {
            const float3 ndc = clip.xyz / clip.w;
            b.minDepth = min(b.minDepth, ndc.z);
            b.lo = min(b.lo, ndc.xy);
            b.hi = max(b.hi, ndc.xy);
        }
    }
    if (sidesOnly) allOut &= 15u;
    b.inFrustum = allOut == 0u;
    return b;
}

// Behind the pyramid of this frame's early depth: the rectangle (a pixel wider each side, for
// the jitter and rounding) read at the level where it spans at most 2x2 texels, whose maximum
// is the farthest depth drawn over it. The box's nearest point is farther still. `size` is
// the frame's in pixels; level L texel = 2^(L+1) pixels square.
bool occluded(Texture2D<float> hiz, uint2 frameSize, uint levels, Bounds b)
{
    if (b.nearCross) return false;
    const int2 size = int2(frameSize);
    const float2 pmin = (b.lo * 0.5 + 0.5) * float2(size);
    const float2 pmax = (b.hi * 0.5 + 0.5) * float2(size);
    const int2 r0 = clamp(int2(floor(pmin)) - 1, int2(0), size - 1);
    const int2 r1 = clamp(int2(floor(pmax)) + 1, int2(0), size - 1);
    uint level = 0u;
    for (; level + 1u < levels; ++level)
    {
        const int s = int(level + 1u);
        if ((r1.x >> s) - (r0.x >> s) <= 1 && (r1.y >> s) - (r0.y >> s) <= 1) break;
    }
    const int s = int(level + 1u);
    uint w, h, count;
    hiz.GetDimensions(level, w, h, count);
    const int2 last = int2(int(w) - 1, int(h) - 1);
    const int2 t0 = min(r0 >> s, last), t1 = min(r1 >> s, last);
    const float far = max(max(hiz.Load(int3(t0.x, t0.y, int(level))), hiz.Load(int3(t1.x, t0.y, int(level)))),
                          max(hiz.Load(int3(t0.x, t1.y, int(level))), hiz.Load(int3(t1.x, t1.y, int(level)))));
    return b.minDepth - 1e-6 > far;
}

// What a meshlet test needs of its draw, the same for every meshlet of it.
struct MeshletView
{
    float4x4 viewProj;
    float4 c0, c1, c2, c3;   // the instance's world matrix, its columns
    float3 cameraLocal;      // the camera in the mesh's space
    bool cone;               // test the normal cone: a one-sided draw whose matrix keeps winding
    bool frustum;
    bool occlusion;          // late phase only
    uint2 frameSize;
    uint hizLevels;
};

// A draw's meshlet test setup: the camera in mesh space (the world matrix inverted) and
// whether its cone test is valid. Facing is affine-invariant for a matrix of positive
// determinant (the triangle and the camera map together); a mirroring matrix flips it, so its
// draws skip the cone test.
MeshletView meshletView(float4x4 viewProj, float4 c0, float4 c1, float4 c2, float4 c3, float3 camera, uint flags,
                        bool oneSided, uint2 frameSize, uint hizLevels)
{
    MeshletView v;
    v.viewProj = viewProj;
    v.c0 = c0;
    v.c1 = c1;
    v.c2 = c2;
    v.c3 = c3;
    // inverse(M) * (camera - t) by Cramer's rule, M's columns a, b, c.
    const float3 d = camera - c3.xyz;
    const float3 a = c0.xyz, b = c1.xyz, c = c2.xyz;
    const float det = dot(a, cross(b, c));
    v.cameraLocal = det != 0.0 ? float3(dot(d, cross(b, c)), dot(d, cross(c, a)), dot(d, cross(a, b))) / det : float3(0.0);
    v.cone = (flags & kCullCone) != 0u && oneSided && det > 0.0;
    v.frustum = (flags & kCullFrustum) != 0u;
    v.occlusion = (flags & kCullOcclusion) != 0u;
    v.frameSize = frameSize;
    v.hizLevels = hizLevels;
    return v;
}

// A meshlet is drawn unless its normal cone faces wholly away from the camera, its sphere's
// box is outside the frustum, or (late phase) the pyramid hides that box.
bool meshletVisible(MeshletGpu m, MeshletView v, Texture2D<float> hiz)
{
    if (v.cone && m.cone.w < 1.0)
    {
        const float3 d = m.sphere.xyz - v.cameraLocal;
        if (dot(d, m.cone.xyz) >= m.cone.w * length(d) + m.sphere.w) return false;
    }
    if (!v.frustum) return true;
    const float3 r = float3(m.sphere.w);
    const Bounds b = boxBounds(v.viewProj, v.c0, v.c1, v.c2, v.c3, m.sphere.xyz - r, m.sphere.xyz + r, false);
    if (!b.inFrustum) return false;
    return !(v.occlusion && occluded(hiz, v.frameSize, v.hizLevels, b));
}
