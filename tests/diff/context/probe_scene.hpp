// The probe scene: the axis-aligned box every wavefront probe traces, and
// the fused-loop scene constants whose static_asserts are the survival
// argument.
//
// LIFTED VERBATIM out of gpu_probe_context.cpp's two file-scope anonymous
// namespaces, comments and asserts included. It had to move first: five of
// that file's nine probe groups read these, so nothing else in it could be
// split while they stayed private to one translation unit.
//
// THE static_assertS MOVED WITH THE CONSTANTS THEY ASSERT ABOUT, and are
// now checked in every translation unit that includes this header rather
// than in one. That is strictly more coverage. wf_intersect.comp cites
// them by name twice, in the argument for why its ray tMin is 0, and both
// citations now name this file.
#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace ohao::diff::probe_scene {
/// wf_generate.comp's local_size_y. WavefrontStage's Fixed group-count
/// source now supports a genuine 3-D dispatch (groups/groupsY/groupsZ, not
/// just groups), but this probe's own generate dispatch (see
/// generate.setGroupCount below) only ever sets `groups` and leaves
/// `groupsY`/`groupsZ` at Fixed's default of 1 -- i.e. it still dispatches
/// (groupCountX, 1, 1) -- so a fixed dispatch of that shader covers exactly
/// this many pixel rows. See the height check below. Widening this probe to
/// a genuine 2-D dispatch (e.g. 64x48) is a possible follow-up; it is not
/// done here because this probe's expected values (throughput, per-bounce
/// PathRng parity, live counts) are all calibrated to 512 paths at the
/// current resolution.
constexpr uint32_t kFusedLoopGenerateLocalY = 8;

/// wf_generate.comp's local_size_X, which is a DIFFERENT number from
/// local_size_y even though both are 8 today. The two were one constant
/// until a review pointed out that `kFusedLoopGenerateLocalY` was being used
/// as the group-count divisor for the X axis (`width / ...`) as well as the
/// height requirement -- so a change to local_size_x alone would have left
/// the dispatch covering fewer pixel columns than the image has, silently,
/// with the uncovered paths never generated and every downstream count
/// quietly short. Split so that each axis's constant is used only for its
/// own axis.
constexpr uint32_t kFusedLoopGenerateLocalX = 8;

/// Half-extent of the closed box the loop bounces inside. Small enough that
/// its space diagonal is far inside wf_intersect.comp's tMax, large enough
/// that the primary rays' spread is nowhere near degenerate. A power of two
/// on purpose: the box's faces are then at exactly representable
/// coordinates and each face's cross(v1 - v0, v2 - v0) is exactly
/// (+/-4E^2, 0, 0) up to axis permutation, so the geometric normal comes out
/// of normalize() with two bit-exact zero components.
constexpr float kFusedLoopBoxHalfExtent = 4.0f;

/// wf_intersect.comp's ray tMax, mirrored here because the derivation above
/// compares the box's longest chord against it. Change one and this guard
/// stops meaning what it says.
constexpr float kFusedLoopRayTMax = 1000.0f;

/// wf_scatter.comp's ray-origin epsilon offset along the geometric normal,
/// mirrored here for the same reason: the induction needs it to be smaller
/// than the half-extent, or the "next origin is still inside" step fails --
/// AND larger than float resolution at the box's scale, or the offset
/// rounds away to nothing and the "next origin is still inside" step fails
/// the other way (see kFusedLoopScatterOriginOffsetMinBound below).
constexpr float kFusedLoopScatterOriginOffset = 1e-4f;

/// The camera, which must sit strictly inside the box for the induction's
/// base case. These are the values actually pushed to wf_generate.comp
/// below, so the guard checks the camera the probe really uses.
constexpr float kFusedLoopCameraX = 0.0f;
constexpr float kFusedLoopCameraY = 0.0f;
constexpr float kFusedLoopCameraZ = 0.0f;
constexpr float kFusedLoopTanHalfFov = 0.2f;

/// sqrt(3), to four more digits than float can hold -- the box's space
/// diagonal is 2*E*sqrt(3).
constexpr float kSqrt3 = 1.7320508075688772f;

constexpr float kFusedLoopAbs(float v) { return v < 0.0f ? -v : v; }

/// The longest distance any ray can travel inside the box: its space
/// diagonal. Every committed hit is at t* <= this.
constexpr float kFusedLoopBoxDiagonal() { return 2.0f * kFusedLoopBoxHalfExtent * kSqrt3; }

/// Lower bound on kFusedLoopScatterOriginOffset. With wf_intersect.comp's
/// tMin at exactly 0 (see that shader's comment), the ENTIRE
/// self-intersection guarantee rests on the scatter offset being large
/// relative to float resolution at the box's scale -- not merely small
/// relative to the half-extent, which is a completely different, unrelated
/// bound (see the comment above kFusedLoopScatterOriginOffset). A face at
/// |coordinate| == E has an ulp of E * epsilon(); if the offset is at or
/// below that, `q + offset * N` rounds back to `q` exactly in float, and
/// the next ray origin lands ON the surface with tMin == 0 -- ray-tracing
/// APIs make no promise about a ray whose origin is exactly on a triangle
/// it did not just leave a genuine distance from, and a self-intersection
/// there would look like a path randomly dying via check 16.
///
/// 8x is a generous, round margin over the 1-ulp threshold where the
/// guarantee actually first breaks -- not a tight bound.
constexpr float kFusedLoopScatterOriginOffsetMinBound() {
    return 8.0f * kFusedLoopBoxHalfExtent * std::numeric_limits<float>::epsilon();
}

constexpr float kFusedLoopCameraMaxAbsCoord() {
    float m = kFusedLoopAbs(kFusedLoopCameraX);
    if (kFusedLoopAbs(kFusedLoopCameraY) > m) m = kFusedLoopAbs(kFusedLoopCameraY);
    if (kFusedLoopAbs(kFusedLoopCameraZ) > m) m = kFusedLoopAbs(kFusedLoopCameraZ);
    return m;
}

// These are exactly the hypotheses the survival induction in this section's
// header rests on, other than `maxBounces >= 1` (checked at RUNTIME below,
// against the caller's actual argument, where a zero-bounce run is rejected
// alongside this probe's other dispatch-shape requirements):
//
//   1. The box's space diagonal fits inside wf_intersect.comp's tMax, so no
//      exit hit is ever rejected as too far.
//   2. wf_scatter.comp's origin offset is smaller than the half-extent, so
//      stepping off a face lands strictly inside the box rather than
//      through the opposite one.
//   3. wf_scatter.comp's origin offset is larger than float resolution at
//      the box's scale, so it survives rounding and the next ray origin is
//      not left sitting exactly on the surface it just left (tMin == 0 --
//      see wf_intersect.comp and kFusedLoopScatterOriginOffsetMinBound).
//   4. The camera is strictly inside the box, which is the induction's base
//      case.
//
// Every one of these is a compile-time constant -- unlike the staircase
// scene this box replaced, NONE of them depends on `maxBounces`, since the
// induction is uniform in the bounce count (a scene that survives one
// bounce survives a thousand) -- so they are asserted at BUILD time,
// unconditionally, rather than only when this probe happens to run. A
// future edit to E, to wf_intersect.comp's mirrored tMax, or to
// wf_scatter.comp's mirrored offset fails the build instead of quietly
// turning check 16's hard `== kCapacity` equality into something that
// merely happens to hold on this run.
static_assert(kFusedLoopBoxDiagonal() <= kFusedLoopRayTMax,
              "fused-loop box's space diagonal must fit inside wf_intersect.comp's ray tMax, or "
              "some exit hit is rejected as too far and the survival induction's step (b) fails");
static_assert(kFusedLoopScatterOriginOffset < kFusedLoopBoxHalfExtent,
              "fused-loop scatter origin offset must be smaller than the box half-extent, or "
              "stepping off a face can land through the opposite one");
static_assert(kFusedLoopScatterOriginOffset > kFusedLoopScatterOriginOffsetMinBound(),
              "fused-loop scatter origin offset must exceed float resolution at the box's scale "
              "(see kFusedLoopScatterOriginOffsetMinBound), or it rounds away to nothing and the "
              "next ray origin lands ON the surface with wf_intersect.comp's tMin == 0");
static_assert(kFusedLoopCameraMaxAbsCoord() < kFusedLoopBoxHalfExtent,
              "fused-loop camera must sit strictly inside the box, which is the survival "
              "induction's base case");

/// Fills `positions` (3 floats per vertex) and `indices` (3 uints per
/// triangle) with the CLOSED axis-aligned box [-halfExtent, halfExtent]^3:
/// six quads, twelve triangles, twenty-four vertices (each face owns its own
/// four, so no face's winding is constrained by a neighbour's).
///
/// Every face is wound so that cross(v1 - v0, v2 - v0) points OUT of the
/// box. That is deliberate and load-bearing for the normal check: a ray
/// inside the box hits each face from behind its winding-order normal, so
/// wf_intersect.comp's "flip the geometric normal to oppose the incoming
/// ray" step must actually fire on every single hit. Wound inward, the flip
/// would be a no-op on every hit and its absence would be invisible.
///
/// Right-handedness of the (u, v, outward) triple is what makes the winding
/// come out that way: for the +axis face of axis k, (u, v) = ((k+1)%3,
/// (k+2)%3) satisfies e_u x e_v = +e_k, and the pair is swapped for the
/// -axis face so that the cross product flips with the face.
///
/// The face planes are exactly +/-halfExtent on one axis and the edge
/// vectors are exactly axis-aligned, so cross(v1 - v0, v2 - v0) is exactly
/// (+/-4*halfExtent^2) on that axis and exactly 0 on the other two -- which
/// is what lets the normal check assert the two off-axis components are
/// bit-exactly zero.
void buildAxisAlignedBoxGeometry(float halfExtent, std::vector<float>& positions,
                                 std::vector<uint32_t>& indices);

}  // namespace ohao::diff::probe_scene
