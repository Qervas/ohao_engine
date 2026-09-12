// STAGE 3 -- THE PROJECTION, AND WHY THE BOUNDARY TERM NEEDS ITS JACOBIAN.
//
// The boundary pass is a SCREEN-SPACE integral. It walks a silhouette edge
// across the pixel grid and returns dL/d(screen position), 2 floats per
// vertex. Everything Stage 3 has gated so far stops there -- checks 57-62
// optimise screen positions, or parameters of screen positions, and the
// Stage 3 results note records the gap as a deviation in as many words:
// "orthographic only (no projection Jacobian)". Under an orthographic camera
// screen space IS world space up to a constant, so the gap is invisible; it
// is not a small camera-setup detail but the missing link between a
// screen-space derivative and a mesh anyone would want to optimise.
//
// This closes it, and it closes it the same way the parameterisation did:
// with a pullback. dL/d(world) = J_proj^T dL/d(screen), where J_proj is the
// 2x3 Jacobian of the pinhole map at that vertex. The boundary pass is not
// touched and learns nothing -- it is handed screen positions and returns
// screen gradients, exactly as before. The chain is
//
//     theta --[parameterisation]--> world --[projection]--> screen
//     dL/dtheta <--[pullback]-- dL/dworld <--[pullback]-- dL/dscreen
//
// and each arrow is a separate object with a separate finite-difference
// oracle, because a bug in a composed chain is only attributable if the
// links are gated apart.
//
// THE PINHOLE MAP. Camera space is p_c = R (p_w - eye), with R's rows the
// camera's right/up/back axes, so the camera looks down -z_c and a point in
// FRONT of it has z_c < 0. Then
//
//     u = -fx * x_c / z_c + cx        v = -fy * y_c / z_c + cy
//
// and the 2x3 Jacobian with respect to camera space is
//
//     du/dp_c = ( -fx/z_c,   0,       fx x_c / z_c^2 )
//     dv/dp_c = (  0,       -fy/z_c,  fy y_c / z_c^2 )
//
// with the world-space Jacobian following as (that) * R.
//
// WHAT THE THIRD COLUMN IS FOR. Under an orthographic camera it is exactly
// zero: depth does not affect where a point lands, so no amount of gradient
// can recover a motion along the view direction -- the parameter is not
// merely hard to fit, it is UNIDENTIFIABLE. Perspective is what makes that
// column nonzero, and it is nonzero precisely because moving away shrinks
// the projected shape. That is the property the geometry gate for this file
// turns into a test.
//
// NEAR PLANE. A vertex at or behind the eye has no projection -- z_c -> 0
// sends u, v to infinity and the Jacobian with them. `project` REFUSES
// rather than producing a large finite number, because a large finite
// number is what an optimiser would happily descend.
#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace ohao::diff {

/// A pinhole camera, and the pullback from screen gradients to world ones.
class PinholeProjection {
public:
    /// A vertex must be at least this far in front of the eye, in camera-z,
    /// to have a projection at all.
    static constexpr double kNearMargin = 1e-3;

    /// Camera axes from an eye, a target and an up hint. Returns false if
    /// the three are degenerate (eye at the target, or up parallel to the
    /// view direction), which no orthonormal basis can be built from.
    [[nodiscard]] bool setLookAt(const std::array<float, 3>& eye,
                                 const std::array<float, 3>& target,
                                 const std::array<float, 3>& upHint);

    /// Focal lengths in pixels and the principal point. Both focal lengths
    /// must be nonzero.
    [[nodiscard]] bool setIntrinsics(float fx, float fy, float cx, float cy);

    /// world (3 floats per vertex) -> screen (2 floats per vertex).
    /// Returns false, and leaves `outScreen` empty, if any vertex is not
    /// strictly in front of the eye.
    [[nodiscard]] bool project(const std::vector<float>& worldPositions,
                               std::vector<float>& outScreen) const;

    /// dL/d(world) from dL/d(screen), evaluated at `worldPositions`.
    /// `screenGradients` is 2 floats per vertex; the result is 3. Empty on
    /// a length mismatch or a vertex behind the near plane.
    [[nodiscard]] std::vector<float> pullback(const std::vector<float>& worldPositions,
                                              const std::vector<float>& screenGradients) const;

    /// dL/d(eye) from dL/d(screen), for a camera that TRANSLATES with its
    /// orientation held fixed. 3 floats; empty on a length mismatch or a
    /// vertex behind the near plane.
    ///
    /// THE DERIVATIVE IS THE NEGATED SUM OF THE WORLD ONES, and that is not a
    /// shortcut but the geometry: camera space is p_c = R(p_w - eye), so
    /// moving the eye by +d and moving every world point by -d produce the
    /// identical camera-space configuration and therefore the identical
    /// image. Hence d(screen)/d(eye) = -SUM_v d(screen)/d(p_v), and the
    /// pullback follows.
    ///
    /// ORIENTATION HELD FIXED, which is a real restriction and not an
    /// oversight. `setLookAt` derives R from the eye AND the target, so a
    /// look-at camera that moves also ROTATES, and its derivative carries a
    /// second term this does not. A caller wanting that composite must
    /// parameterise the rotation too -- which is a different object, with a
    /// different Jacobian, and is not this. The unit test pins the
    /// distinction by translating the eye and the target TOGETHER, which is
    /// the motion that leaves R untouched.
    [[nodiscard]] std::vector<float> pullbackToEyeTranslation(
        const std::vector<float>& worldPositions,
        const std::vector<float>& screenGradients) const;

    /// dL/d(camera ORIENTATION) from dL/d(screen), for a rotation of the
    /// camera's basis about the WORLD axes through the eye. 3 floats; empty
    /// on a length mismatch or a vertex behind the near plane.
    ///
    /// THIS IS THE SECOND TERM pullbackToEyeTranslation says it does not
    /// carry. Together the two cover a rigid camera pose: translation moves
    /// the eye with R fixed, rotation turns R with the eye fixed, and a
    /// look-at camera that moves its target does both at once.
    ///
    /// THE PARAMETERISATION, stated because there are several and they
    /// disagree by signs. `theta` rotates the camera's basis vectors about
    /// the world axes, right-handed, through the eye:
    ///
    ///     R(theta) = R * exp(-[theta]_x),   so   p_c(theta) = R exp(-[theta]_x) (p_w - eye)
    ///
    /// and therefore d(p_c)/d(theta_k) = -R (e_k x (p_w - eye)). The eye does
    /// not move, because that offset is what the rotation acts on.
    ///
    /// WORLD AXES RATHER THAN THE CAMERA'S OWN, which is the less obvious
    /// choice (a user steering a camera thinks in pan/tilt/roll about its own
    /// axes). The reason is the ORACLE: `setLookAt` is the only way to set a
    /// basis, so the finite-difference test has to build the perturbed camera
    /// by rotating the target and the up hint -- and those are world vectors.
    /// A world-axis parameterisation lets the test CONSTRUCT the perturbation
    /// exactly; a camera-axis one would have it convert between frames first,
    /// which is a second piece of untested arithmetic standing between the
    /// oracle and the thing it checks. Pan/tilt/roll is this composed with
    /// R^T, and that composition belongs to whoever wants those names.
    [[nodiscard]] std::vector<float> pullbackToEyeRotation(
        const std::vector<float>& worldPositions,
        const std::vector<float>& screenGradients) const;

    /// The 2x3 Jacobian at one world point, row-major (du/dx du/dy du/dz,
    /// then dv/...). Empty if the point is not in front of the eye. Exposed
    /// so a test can look at the third column directly -- the one that
    /// separates perspective from orthographic.
    [[nodiscard]] std::vector<double> jacobian(double wx, double wy, double wz) const;

    [[nodiscard]] bool valid() const noexcept { return m_hasBasis && m_hasIntrinsics; }

private:
    std::array<double, 3> m_eye{0.0, 0.0, 0.0};
    // Rows of R: right, up, back. Camera looks down -back.
    std::array<double, 9> m_r{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    double m_fx = 1.0;
    double m_fy = 1.0;
    double m_cx = 0.0;
    double m_cy = 0.0;
    // TWO flags, not one. A camera is usable only when BOTH halves have been
    // set, and the two setters can be called in either order -- a single
    // flag would record whichever ran last.
    bool m_hasBasis = false;
    bool m_hasIntrinsics = false;

    [[nodiscard]] std::array<double, 3> toCamera(double wx, double wy, double wz) const;
};

}  // namespace ohao::diff
