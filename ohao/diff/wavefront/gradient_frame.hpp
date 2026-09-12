// What ONE differentiable render is, as data, and the push constants it
// becomes. Stage 5, item 1: the third slice of the orchestration move.
//
// WHY THIS IS A LIBRARY HEADER AND NOT TEST-SIDE KNOWLEDGE. The three types
// below -- a camera basis, a scatter material, and the description of a
// gradient render -- had all lived in tests/diff/gpu_probe_context.hpp, in
// namespace ohao::diff, which is where they were first needed rather than
// where they belong. None of them is about testing: the camera basis is
// byte-matched to wf_generate.comp's Push block, the material to the tail of
// WavefrontLoop::ScatterPush, and the frame is the full set of things any
// caller must decide before `shaders/diff/` will run. An engine needs every
// one of them, and needs them without linking a test.
//
// THE ASSEMBLY IS THE PART WORTH MOVING, not the structs. Turning a frame
// into the two `WavefrontLoop::Config`s the forward and replay
// instantiations take carries several decisions that are easy to get wrong
// and impossible to see once wrong:
//
//   * which fields go to the REPLAY run only (the arena, the adjoint seed),
//   * which go to BOTH (everything that steers the traversal),
//   * and that the second set must AGREE between them, because the two
//     instantiations walk one path only while every steering field matches.
//
// That last one was a comment in a test file. It is now
// `steeringFieldsAgree`, which diff_unit_tests asserts -- so an edit that
// pushes a steering field to only one run fails at once, instead of
// producing a gradient for a path the forward run did not take.
#pragma once

#include "diff/wavefront/wavefront_loop.hpp"

#include <cstdint>

namespace ohao::diff {

/// Camera basis, byte-layout-matched to wf_generate.comp's Push block (see
/// `GenerateCameraPush` below). Kept as plain float arrays rather than
/// glm::vec3 so this header does not need to pull in glm for a POD parameter
/// block.
struct WavefrontGenerateCamera {
    float origin[3]{0.0f, 0.0f, 0.0f};
    float forward[3]{0.0f, 0.0f, -1.0f};
    float right[3]{1.0f, 0.0f, 0.0f};
    float up[3]{0.0f, 1.0f, 0.0f};
    float tanHalfFov{0.2f};
};

/// The surface parameters that are NOT the base colour (`albedo`), byte-
/// matched to the tail of WavefrontLoop::ScatterPush.
///
/// The defaults are the PURE LAMBERTIAN configuration: `specularWeight` 0
/// removes the specular lobe entirely (both from f and from the lobe
/// selection probability), leaving f = albedo/pi sampled by a cosine
/// hemisphere, whose estimator weight f*cos/pdf is exactly `albedo`.
struct WavefrontScatterMaterial {
    float roughness{1.0f};
    float metallic{0.0f};
    float specularWeight{0.0f};
};

/// wf_generate.comp's workgroup size. TWO CONSTANTS, not one, even though
/// both are 8 today: they were a single constant until a review found the Y
/// one being used as the group-count divisor for the X axis as well as for
/// the height requirement, so a change to local_size_x alone would have left
/// the dispatch covering fewer pixel columns than the image has -- silently,
/// with the uncovered paths never generated and every downstream count
/// quietly short. Split so each axis's constant is used only for its own
/// axis.
inline constexpr std::uint32_t kGenerateLocalX = 8u;
inline constexpr std::uint32_t kGenerateLocalY = 8u;

/// The X group count for a film `width` wide. The dispatch is
/// (groupCountX, 1, 1) -- wf_generate.comp covers kGenerateLocalY rows per
/// group in Y, which is why the film height is pinned to that number rather
/// than being a second group count.
[[nodiscard]] constexpr std::uint32_t generateGroupCountX(std::uint32_t width) noexcept {
    return width / kGenerateLocalX;
}

/// wf_generate.comp's Push block, 80 bytes.
///
/// The padding is explicit rather than implied by alignment: std430 pads a
/// vec3 to 16 bytes, and a struct that relied on the C++ compiler happening
/// to agree would be a silent wrong-field push rather than a compile error.
struct GenerateCameraPush {
    float origin[3];
    float pad0;
    float forward[3];
    float pad1;
    float right[3];
    float pad2;
    float up[3];
    float pad3;
    std::uint32_t width;
    std::uint32_t height;
    float tanHalfFov;
    std::uint32_t capacity;
};
static_assert(sizeof(GenerateCameraPush) == 80,
              "GenerateCameraPush must match wf_generate.comp's Push block layout");

/// Which of the two instantiations of `shaders/includes/diff/traverse.glsl` a
/// configuration is for. Spec 6.2's one-source/two-instantiations property:
/// the same traversal, hooked differently.
enum class GradientRun : std::uint32_t {
    /// Writes the film. Given no arena and no adjoint seed.
    Forward = 0u,
    /// Scatters gradients. Given both.
    Replay = 1u,
};

/// Everything ONE differentiable render needs that is not a Vulkan handle.
///
/// A frame is the same for both runs. The difference between them is
/// entirely in `loopConfigFor`, which is the point of keeping this one
/// struct rather than two.
struct GradientFrame {
    WavefrontGenerateCamera camera{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t bounces{0};
    std::uint32_t iterationSeed{0};

    /// The base colour being differentiated, and the rest of the surface.
    float albedo{0.5f};
    WavefrontScatterMaterial material{};

    /// 0 = base colour, 1 = roughness, 2 = metallic, 3 = emission,
    /// 4 = emission texture. Matches DIFF_PARAM_* in
    /// shaders/includes/diff/bsdf_adjoint.glsl.
    std::uint32_t diffParam{0};

    /// The gradient arena, as the shader addresses it: a total float count
    /// (0 disables every gradient write) and the differentiated parameter's
    /// own offset within it, in floats.
    std::uint32_t gradArenaFloats{0};
    std::uint32_t gradParamOffset{0};

    /// dL/dpixel, as a float COUNT. 0 means no seed is bound, which the
    /// traversal reads as the sum-of-film objective.
    std::uint32_t adjointSeedFloats{0};

    /// Pixels in the caller-owned film buffer; 0 disables accumulation.
    std::uint32_t filmPixelCount{0};

    /// Uniform self-emitted radiance, and the texture that REPLACES it when
    /// a width and height are given. Both are properties of the SCENE, so
    /// both runs get them.
    float emission{0.0f};
    std::uint32_t emissionTexWidth{0};
    std::uint32_t emissionTexHeight{0};
    std::uint32_t emissionTexChannels{0};
    float emissionUvScaleU{0.0f};
    float emissionUvScaleV{0.0f};
    float emissionUvBiasU{0.0f};
    float emissionUvBiasV{0.0f};

    /// Hold the sampled directions still. Spec 6.3 lists them as NOT
    /// differentiated, so a finite difference that re-runs the sampler
    /// measures the adjoint's quantity PLUS the movement of every direction.
    /// When set, both runs sample from `samplingAlbedo`/`samplingMaterial`
    /// whatever the evaluated material above says.
    bool freezeSampling{false};
    float samplingAlbedo{0.0f};
    WavefrontScatterMaterial samplingMaterial{};
};

/// The camera push block for a frame. `capacity` is the wavefront's, which
/// is the frame's pixel count for a one-sample-per-pixel render but is the
/// BUFFER's property and so is passed separately rather than derived.
[[nodiscard]] GenerateCameraPush generatePush(const GradientFrame& frame,
                                              std::uint32_t capacity);

/// The loop configuration for one of the two instantiations.
///
/// WHAT DIFFERS BETWEEN THE TWO, and nothing else does:
///
///   * `gradArenaFloats` / `gradAlbedoOffset` are given to the REPLAY run
///     alone. The forward run's stay 0, which disables every gradient write
///     in its traversal -- so "the forward pass wrote no gradient" is
///     enforced by a push constant as well as by its hook being the film
///     write.
///   * `adjointSeedFloats` likewise. dL/dpixel is a property of the
///     OBJECTIVE, and the forward hook has no use for it: its job is to
///     write the film, and the film does not depend on what will later be
///     differentiated. Pushing it to both would be harmless today (the
///     forward hook never calls diffAdjointSeed) and a standing invitation
///     to make the film depend on the loss, which spec 4.6 forbids in that
///     exact direction.
///
/// EVERYTHING ELSE IS THE SAME IN BOTH, including the emission and the
/// emission texture -- those are properties of the scene this loop renders,
/// not of which run it is -- and including every field that steers the
/// traversal. `steeringFieldsAgree` below is that requirement as a
/// predicate.
[[nodiscard]] WavefrontLoop::Config loopConfigFor(const GradientFrame& frame, GradientRun run);

/// True when two configurations agree on every field that STEERS THE
/// TRAVERSAL -- the sampling material and its override flag, the seed, the
/// differentiated parameter, and the emission the forward hook adds.
///
/// THE INVARIANT THIS IS FOR. The forward and replay instantiations must
/// walk ONE path. They do that by construction -- one source, two hooks,
/// spec 6.2 -- but only while the push constants that decide where the path
/// goes are identical. `diffParam` gates the tangent update in path state;
/// the sampling material gates every direction drawn; the seed gates the RNG
/// stream; the emission changes the film the replay run is the derivative
/// OF. A field pushed to one run and not the other produces a gradient for a
/// path the forward run never took, and every check that compares the two
/// against each other would still pass, because both sides would move.
[[nodiscard]] bool steeringFieldsAgree(const WavefrontLoop::Config& a,
                                       const WavefrontLoop::Config& b);

}  // namespace ohao::diff
