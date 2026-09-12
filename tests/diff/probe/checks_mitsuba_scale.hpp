// Check 72: THE ABSOLUTE RADIOMETRIC SCALE, against a closed form an
// INDEPENDENT RENDERER has confirmed. Gate 4, spec 8.4, leg 2.
//
// WHAT EVERY OTHER GATE HERE CANNOT SEE. Checks 33-34 compare the GPU film
// against our own CPU reference integrator. Check 37 compares the scattered
// gradient against a finite difference of our own film. Checks 60/62/63
// compare against a supersampled version of our own coverage. Each of those
// is a strong check of CONSISTENCY, and between them they have caught a sign
// error, a 3x orientation error and a factor-of-two pixel seam.
//
// None of them can see a UNIFORM RESCALING. Multiply the reconstructed
// environment radiance, or the Lambertian 1/pi, or the camera's sample
// weight by any constant, and the GPU film and the CPU reference move
// together (they share the derivation), the finite difference and the
// analytic gradient move together (the second is the derivative of the
// first), and the supersampled coverage does not involve radiance at all.
// Every check stays green and every image is wrong by that constant.
//
// THE SCENE IS THE ONE CONFIGURATION WHOSE ANSWER IS KNOWN EXACTLY. An ideal
// Lambertian surface filling the frame, lit by a CONSTANT environment of
// radiance L, reflects
//
//     L_o = integral f(w) L cos(theta) dw = (a/pi) * L * pi = a * L
//
// so every pixel reads a*L with no quadrature error of any kind to argue
// about, and
//
//     d(mean pixel) / d(albedo) = L.
//
// THREE-WAY, not two-way. tests/diff/tools/mitsuba_gate.py computes the same
// two numbers in Mitsuba 3 and compares all three, and that leg is what makes
// the closed form above more than our own arithmetic restated. Its leg 1
// (mitsuba against the closed form) runs first and alone, because if those
// two disagree the SCENE DESCRIPTION is wrong and nothing has been learned
// about this renderer. This check is leg 2; it prints the machine-readable
// MITSUBA-GATE line that script parses, and it also reaches its own verdict
// against the closed form so that the probe alone catches a regression
// without needing Python or a CUDA device present.
//
// WHY THE GRADIENT HERE IS NOT AN INDEPENDENT SECOND MEASUREMENT, stated
// rather than implied. At one bounce the film is EXACTLY linear in the
// albedo: the MIS weights and both sampling densities are independent of it
// under pure Lambert, so film = a * X and the scattered gradient is that same
// X. a * gradient == film is therefore an identity, and this check asserts it
// to float precision rather than pretending two numbers were measured. The
// content of the gate is the SCALE, which is what the identity propagates
// from one quantity to the other.
//
// THE TOLERANCE IS A SAMPLING ALLOWANCE, not a precision one: one sample per
// pixel is the probe's hard constraint, so the estimate is an average over
// seeds and the gate is a multiple of the standard error MEASURED ACROSS
// those seeds, with a pre-registered resolution floor that refuses a verdict
// the sampling could not have resolved. Same two-part structure as check 37.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkMitsubaScale(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
