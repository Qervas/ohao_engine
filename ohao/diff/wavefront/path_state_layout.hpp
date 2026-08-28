#pragma once

#include "diff/grad/arena_layout.hpp"

#include <cstdint>

namespace ohao::diff {

/// One scalar field per enumerator. Deliberately scalar rather than vec3:
/// a wavefront stage that only needs throughput should read one dense run,
/// not stride across interleaved path structs.
enum class PathStateField : std::uint32_t {
    OriginX, OriginY, OriginZ,
    DirX, DirY, DirZ,
    ThroughputR, ThroughputG, ThroughputB,
    RadianceR, RadianceG, RadianceB,
    PixelIndex,    // bit-cast uint
    SampleIndex,   // bit-cast uint
    Bounce,        // bit-cast uint
    Alive,         // bit-cast uint, 0 or 1
    HitT,          // intersect-stage output: hit distance, -1 on miss (Task 5)
    // Intersect-stage output: the FORWARD-FACING geometric normal of the
    // committed hit, in world space, unit length (Stage 0b-2b Task 1).
    // "Forward-facing" means already flipped to oppose the incoming ray
    // (dot(Normal, Dir) <= 0), which is the orientation every BSDF and the
    // scatter stage's hemisphere basis needs; the raw winding-order normal
    // is deliberately not what is stored.
    //
    // UNDEFINED ON A MISS: wf_intersect.comp leaves these three fields
    // untouched when HitT is set to -1 (no surface, so no normal to write).
    // Any read of Normal must gate on HitT >= 0 first -- on a miss this
    // holds whatever the arena held before that bounce, not a sentinel.
    //
    // Three scalar fields rather than one packed octahedral field. That is a
    // decision, not an oversight -- see shaders/includes/common/encoding.glsl
    // for the packing that was NOT used, and this file's git history / the
    // task-1 report for the reasoning: (a) the probe's normal check compares
    // the stored value against an analytic surface normal, and an exactness
    // assertion has to survive storage untouched -- an octahedral round trip
    // introduces a quantisation error that any future non-axis-aligned scene
    // would make visible, forcing the check's tolerance to be widened to
    // hide the encoding rather than to bound the shader; (b) the saving is
    // one ArenaLayout block in twenty, which for a probe is nothing; and
    // (c) octahedral's |x|+|y|+|z| fold is not differentiable at the
    // octahedron's edges, which is a poor thing to sit underneath a
    // renderer whose entire purpose is gradients flowing through geometry.
    NormalX, NormalY, NormalZ,
    // THE THROUGHPUT TANGENT (Stage 1 Task 3): d(Throughput)/d(theta) for the
    // ONE scalar parameter a run differentiates, carried across bounces by
    // the traversal itself.
    //
    // WHY IT HAS TO BE STORED AT ALL. The film is J = SUM_b T_b * Lr_b, so
    // dJ/dtheta carries a (dT_b/dtheta) * Lr_b term, and
    //
    //     T_{b+1} = T_b * weight_b   =>   T'_{b+1} = T'_b * weight_b
    //                                                + T_b * weight'_b
    //
    // is a RUNNING product-rule accumulation: the value at bounce b depends
    // on every earlier bounce's weight derivative. Stage 1 Task 2 avoided
    // storing it because for a pure Lambertian surface T_b = albedo^b
    // exactly, so dT_b/d(albedo) = (b/albedo) * T_b in closed form. That
    // closed form is Lambert's alone. Once the BSDF weight is
    // f*cos/pdf through a microfacet lobe there is no closed form for the
    // product, and a wavefront stage keeps NOTHING in registers across a
    // dispatch boundary -- so the tangent has to live where the throughput
    // it differentiates lives.
    //
    // Written by the TRAVERSAL (shaders/includes/diff/traverse.glsl), never
    // by a hook: the hook contract forbids a hook writing path state, and
    // both instantiations must update it identically or they walk different
    // paths. Seeded to zero by wf_generate.comp -- T_0 = 1 for every theta,
    // so its derivative is 0 -- and left untouched on the miss path, where
    // the throughput is not decayed either.
    //
    // Three scalar fields rather than one, for ThroughputR/G/B's reason: the
    // tangent of a vec3 is a vec3, and the probe's configurations are grey
    // only by choice of scene, not by anything this layout may assume.
    TangentR, TangentG, TangentB,
    Count
};

/// SoA offsets for `capacity` in-flight paths. Pure -- no Vulkan.
///
/// shaders/includes/diff/path_state.glsl mirrors this layout. The two are a
/// matched pair like PathRng and rng.glsl: change both or neither, and the
/// probe's field round-trip check is what proves they agree.
class PathStateLayout {
public:
    explicit PathStateLayout(std::uint32_t capacity);

    [[nodiscard]] std::size_t block(PathStateField field) const;
    [[nodiscard]] std::uint32_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] const ArenaLayout& arena() const noexcept { return m_arena; }

    static constexpr std::uint32_t kFieldCount =
        static_cast<std::uint32_t>(PathStateField::Count);

private:
    std::uint32_t m_capacity{0};
    ArenaLayout m_arena;
    std::size_t m_blocks[kFieldCount];
};

}  // namespace ohao::diff
