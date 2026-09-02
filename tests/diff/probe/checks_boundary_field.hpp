// Stage 3, check 64: A RADIANCE JUMP THAT VARIES ALONG THE EDGE.
//
// Every boundary check before this one pushes one radiance per side, so the
// jump is a single constant everywhere. The Stage 3 results note records
// that as a deviation -- "the radiances are pushed, not traced" -- and its
// substantive half is not that the numbers are pushed but that the jump
// cannot VARY. This gate is the first half of that closed: the radiance on
// each side is an affine field of screen position, so the jump varies along
// every chord, and the kernel integrates it as a moment rather than as a
// weight times a scalar.
//
// WHY THE ORACLE SURVIVES THE GENERALISATION. Spec 4.1's interior term is
// INTEGRAL (df/dtheta) dx, and these fields depend on screen position but
// NOT on theta. Moving a vertex still changes nothing but which side of the
// edge a point falls on, so the interior term is still exactly zero and a
// supersampled image difference is still purely the boundary integral. That
// is what made this layer addable without giving up the only oracle that has
// ever caught a real defect here.
//
// THE CONTROL IS THE KERNEL'S OWN PREVIOUS FORM: the same run with the field
// gradients zeroed and the constants set to the field at the triangle's
// CENTROID -- the best single constant the earlier kernel could express. It
// must miss the tolerance the varying form meets, or the generalisation is
// present without being necessary.
#pragma once

#include "gpu_probe_context.hpp"

namespace ohao::diff::probe {

bool checkBoundaryField(ohao::diff::GpuProbeContext& ctx);

}  // namespace ohao::diff::probe
