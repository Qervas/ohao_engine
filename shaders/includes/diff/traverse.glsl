#ifndef OHAO_DIFF_TRAVERSE_GLSL
#define OHAO_DIFF_TRAVERSE_GLSL

// ===========================================================================
// THE WAVEFRONT TRAVERSAL -- ONE SOURCE, TWO INSTANTIATIONS
// ===========================================================================
//
// This file is the WHOLE of the scatter stage: its extensions, its includes,
// its bindings, its push-constant block, its local size, its constants and
// its per-invocation traversal. Two `.comp` files include it --
// shaders/diff/wf_scatter.comp (the FORWARD pass) and
// shaders/diff/wf_scatter_replay.comp (the REPLAY pass) -- and neither adds
// anything to it except the body of ONE function, `diffVertexHook`, plus a
// `main` that calls `diffTraverse()`.
//
// WHY THE WHOLE FILE AND NOT JUST THE LOOP BODY. Spec section 6.2:
//
//     "The backward kernel must walk the identical path, consuming the
//      identical RNG values in the identical order. Divergence by a single
//      RNG call means the replayed path is a different path and every
//      gradient is silently wrong -- no crash, no NaN. Therefore the
//      traversal is one piece of source, included twice, with a per-vertex
//      hook the includer defines. Divergence is made structurally impossible
//      rather than prevented by discipline."
//
// "Structurally impossible" is only true if there is nothing left in either
// instantiation that COULD diverge. A shared traversal function whose two
// includers each declared their own bindings and their own Push block would
// still leave two hand-maintained copies of the descriptor interface and of
// the byte layout `ohao::diff::WavefrontLoop::ScatterPush` pushes into both
// -- and a Push block that drifts by one field feeds the traversal a
// different `iterationSeed` or a different `capacity`, which walks a
// different path just as surely as a stray `diffRngNext1D` would. So the
// bindings and the Push block live HERE, once.
//
// What is left in each `.comp` is therefore: `#version`, the include, the
// hook body, and `void main() { diffTraverse(); }`. `diff_gpu_probe.cpp`'s
// `checkTraverseInstantiationTie()` parses both files and REFUSES TO RUN if
// either contains a `layout(...)` declaration, a second `#include`, or any
// function other than those two -- so the "one source" claim is measured at
// startup rather than asserted in this comment.
//
// THE HOOK, AND WHY ITS SIGNATURE IS WHAT IT IS. See `DiffVertex` below.
//
// EVERYTHING BELOW THIS BANNER, down to the `DiffVertex` block, is
// wf_scatter.comp's own file comment, moved here unchanged with the code it
// describes. Where it says "this file" or "this stage" it now means the
// traversal both instantiations share.

#extension GL_EXT_ray_query : require
// Buffer float atomics ONLY -- the FORWARD instantiation's per-vertex hook
// accumulates the film with an atomicAdd on a storage buffer, and the two
// compaction counters are integer atomicAdds in this file. The extension is
// required HERE, not in each includer, so that both instantiations compile
// against the identical feature set. No image atomics anywhere in this
// subsystem: shaders/diff/atomic_probe.comp already proves
// shaderBufferFloat32AtomicAdd accumulates correctly under contention on
// this device, and diff_gpu_probe.cpp refuses to run without that feature.
#extension GL_EXT_shader_atomic_float : require

// Wavefront stage: scatter. Evaluates and importance-samples the surface
// BSDF (shaders/includes/diff/bsdf.glsl -- Lambert + single-scattering GGX),
// multiplies the path's throughput by the resulting estimator weight
// f*cos/pdf, advances the path to its hit point along the sampled direction,
// increments its bounce counter, and re-queues it into the next bounce's
// ring by atomicAdd, mirroring wf_intersect.comp's compaction mechanism
// exactly (every invocation here always re-queues -- nothing terminates in
// scatter yet; Russian roulette would add a condition right where
// wf_intersect.comp's miss branch sits).
//
// The BSDF is NOT written here. It lives in shaders/includes/diff/bsdf.glsl,
// which in turn builds on shaders/includes/material/ggx_aniso.glsl -- the
// same microfacet file the RT pipeline's raygen shaders use -- and is
// observed directly by shaders/diff/bsdf_probe.comp, so diff_gpu_probe.cpp
// can compare f, the pdf and the sampler weight term by term against a CPU
// oracle written from the published formulas. This file's job is the
// wavefront plumbing around that call, not the physics inside it.
//
// RNG reconstruction is the other entire point of this file (see task-6
// brief). A megakernel keeps a path's RNG state in a register across every
// bounce, so draw order is preserved for free. This stage cannot: it is a
// separate vkCmdDispatch every bounce, so nothing survives in registers
// across that boundary. Every invocation therefore rebuilds its RNG from
// (pixelIndex, sampleIndex, iterationSeed) via diffRngForPath and
// fast-forwards it past every earlier bounce's draws
// (bounce * kDrawsPerBounce, where `bounce` comes from path state's Bounce
// field -- never from anything carried over) before drawing this bounce's
// own values. Get the fast-forward count wrong and the path replays a
// DIFFERENT random sequence starting next bounce: a perfectly plausible
// image now, silently wrong gradients when Stage 1's backward pass replays
// it. No crash, no NaN.
//
// Binding 3 (the VERTEX TRACE) is a probe-only diagnostic sink, not part of
// the production wavefront data model: it exists so a black-box GPU probe can
// observe the exact stream and the exact vertex a REAL dispatch produced for
// each path -- reading this file is not proof the GPU executed it bit-exactly
// against ohao::diff::PathRng. See diff_gpu_probe.cpp's per-bounce RNG-parity
// check.
//
// It carried (u1, u2, drawCountAfter) through Stage 0b-2b and now carries all
// FIVE of the bounce's draws plus the origin, direction, throughput and hit
// distance the traversal read out of path state. Those fifteen extra floats
// are what make REPLAY EQUIVALENCE observable: both instantiations of this
// file write this record, from two independent runs at the same seed, and
// diff_gpu_probe.cpp compares them bit for bit. A record holding only the
// first two draws could not see a divergence in draws 3-5, which leaves this
// bounce's u1/u2 identical while moving every LATER bounce's stream.
//
// Both the source count and the destination write are clamped to pc.capacity,
// mirroring wf_intersect.comp's compaction guard exactly. In the current
// probe this is unreachable -- WavefrontBuffers::zero resets counter slots
// to 0 and srcCount never exceeds capacity -- but this shader is
// parameterised (srcQueueBase/srcCountSlot/dstQueueBase/dstCountSlot) for
// reuse across bounces (N->N+1, N+1->N+2, ...), and the queue buffer is
// exactly 2*capacity uints. A caller that reuses a ring without re-zeroing
// its counter would otherwise write past the end of that allocation.
//
// ENVIRONMENT IMPORTANCE SAMPLING (Stage 0b-2b Task 3) is the other thing
// this stage now does per bounce. It draws a direction proportional to the
// environment's sin(theta)-weighted luminance through
// shaders/includes/rt/env_sampling.glsl's sampleEnvMap -- the SAME header
// the RT pipeline's raygen/miss shaders use, so the two pipelines' CDF
// convention (inclusive per-row-normalised conditional, inclusive marginal,
// texel-centre directions, solid-angle pdf) cannot drift -- and writes the
// (direction, pdf) pair to a probe-only sink at binding 6.
//
// Writing that sample to an observable sink is what lets diff_gpu_probe.cpp
// bin a real dispatch's directions and reject them against a host-built
// oracle with a chi-squared test, instead of inferring the sampler's
// correctness from an image. It still does not touch throughput, origin,
// dir or radiance -- but it is no longer unconsumed; see the next paragraph.
//
// NEXT-EVENT ESTIMATION AND MIS (Stage 0b-2b Task 4) is what finally
// CONSUMES that environment sample. The (direction, pdf) pair written to
// binding 6 is not re-drawn and is not gated: the one sampleEnvMap call
// above is the light sample next-event estimation uses, so the draw
// diff_gpu_probe.cpp's chi-squared check bins IS the production draw, by
// construction rather than by convention. (The alternative -- a separate
// NEE draw beside an unconditional probe write -- would leave check 24
// passing at the same statistic while measuring a vestigial sample nothing
// used.) The estimator itself lives in shaders/includes/diff/nee.glsl as
// ONE parameterised function serving BOTH strategies; see that file for why
// it is one function and not the four copies pt_raygen.rgen carries.
//
// Both strategies need a shadow ray, so this stage now binds the same
// acceleration structure wf_intersect.comp traces against (binding 8) and
// issues an inline ray query -- the same GL_EXT_ray_query the sibling stage
// already requires, no new device feature. Their per-sample results go to a
// probe-only sink at binding 7; nothing is accumulated into path state
// here, because Task 4's estimators are accumulated HOST-SIDE from that
// readback. A host accumulator is an oracle INDEPENDENT of the GPU
// accumulation path, which is what this stage's check discipline needs, and
// it keeps the checks valid whatever Task 5 does to film accumulation.
// Task 5 did NOT move them onto the film buffer below, deliberately: checks
// 29-31 would then depend on the very accumulation path they are the
// independent oracle for.
//
// RADIANCE ACCUMULATION INTO A FILM (Stage 0b-2b Task 5) is what turns the
// two per-sample estimators above into an image. Every invocation adds
//
//     throughput_on_arrival * (w_E * E_unweighted + w_B * B_unweighted)
//
// -- the MIS-combined direct-lighting estimate at this vertex, carried by
// the path's throughput BEFORE this bounce's BSDF decay -- into
// film[pixelIndex] at binding 9, by atomicAdd. The throughput used is the
// arrival throughput, not the decayed one, because the estimator already
// contains this vertex's f*cos/p; multiplying by the post-decay value would
// count this bounce's BSDF twice.
//
// The film is CALLER-OWNED, written at a FIXED pixelIndex*3 offset on EVERY
// bounce, and read-modify-written (atomicAdd). See the barrier note above
// the write itself at the end of this file for exactly which barrier orders
// it between bounces -- nothing in this repository tests that ordering.
//
// Binding scheme: state (0, declared by path_state.glsl), queues (1),
// counters (2), debug draws (3), env marginal CDF (4), env conditional CDF
// (5), env samples (6), NEE samples (7), TLAS (8), film (9).

#include "diff/path_state.glsl"
#include "diff/rng.glsl"
#include "diff/bsdf.glsl"
#include "diff/nee.glsl"
#include "pbr_unpack.glsl"

layout(local_size_x = 64) in;

layout(std430, binding = 1) buffer QueueBuffer {
    uint idx[];
} queues;

layout(std430, binding = 2) buffer CounterBuffer {
    uint value[];
} counters;

// Probe-only: kTraceFloats floats per PATH INDEX -- indexed by pathIndex, not
// by queue slot. Slots 0-2 are still (u1, u2, drawCountAfter as
// uintBitsToFloat(rng.draws)); the rest is the vertex. See kTraceFloats below
// for the layout and the tie that holds it to ohao::diff::TraceSlot, and the
// file header for what the record is for. The buffer instance is still named
// `debugDraws` because the host constant, the descriptor slot and three
// checks that predate the widening all address it by that name.
layout(std430, binding = 3) buffer DebugDraws {
    float v[];
} debugDraws;

// --- Environment CDF, READ-ONLY -------------------------------------------
//
// shaders/includes/rt/env_sampling.glsl declares no bindings of its own (the
// layout() lines near its top sit inside a comment describing what a caller
// owes it); the only caller-provided symbols it references are envMarg.data
// and envCond.data. W, H and the integral are already ORDINARY ARGUMENTS of
// sampleEnvMap/pdfEnvMap, so this stage passes its own push-constant fields
// and does not have to adopt the RT pipeline's pc.control/pc.tuning names --
// nothing in the header itself had to change for that. These two buffers do
// have to be declared BEFORE the include, which is why it sits below them
// rather than with the other includes at the top of the file.
//
// Uploaded once by WavefrontBuffers and never written by any dispatch, so
// they are NOT among the buffers WavefrontLoop::record must order against
// itself: its extraBarrierBuffers parameter is for buffers the dispatches
// WRITE.
layout(std430, binding = 4) readonly buffer EnvMarginalCDF {
    float data[];
} envMarg;

layout(std430, binding = 5) readonly buffer EnvConditionalCDF {
    float data[];
} envCond;

#include "rt/env_sampling.glsl"

// Probe-only, like DebugDraws above: 4 floats per PATH INDEX
// (dirX, dirY, dirZ, pdf) as sampleEnvMap returned them for THIS dispatch.
// Written unconditionally -- an environment sample is a property of the
// environment, not of whether this path hit anything -- so a missed path
// still produces one. Task 4's next-event estimator consumes THIS pair:
// there is exactly one sampleEnvMap call in this shader and its result is
// both what is recorded here and what NEE uses, so the samples
// diff_gpu_probe.cpp's chi-squared check bins are the production light
// samples and cannot drift away from them.
layout(std430, binding = 6) buffer EnvSamples {
    float v[];
} envSamples;

// Probe-only, like DebugDraws and EnvSamples: kNeeSampleFloats floats per
// PATH INDEX recording what the two MIS strategies computed for THIS
// dispatch. Written unconditionally (zeros on a miss, see the write below)
// so the host never reads a stale bounce's values out of it.
//
// Both single-strategy estimators and both halves of each sample's MIS
// partition are recorded, rather than just the combined estimator, because
// the checks that matter are (a) BSDF-only and NEE-only converging to the
// same integral -- three estimators, one truth, no shared expected value --
// and (b) the two weights at ONE direction summing to exactly 1. Neither is
// recoverable from a combined radiance value alone.
layout(std430, binding = 7) buffer NeeSamples {
    float v[];
} neeSamples;

// The SAME acceleration structure wf_intersect.comp traces against
// (its binding 5, this stage's 8 -- the index is each stage's own choice).
// Read-only: no dispatch writes an acceleration structure, so this is not
// among the buffers WavefrontLoop::record must order against itself.
layout(binding = 8) uniform accelerationStructureEXT topLevelAS;

// --- THE FILM (Stage 0b-2b Task 5). NOT probe-only ---------------------
//
// 3 floats per PIXEL INDEX (R, G, B), accumulated across bounces by
// atomicAdd. Indexed by psGetPixelIndex, not by path index: the film is the
// image, and a path's pixel is not its path index in general.
//
// The write is atomic rather than a plain store because it is a
// READ-MODIFY-WRITE against the PREVIOUS bounce's value, which exists in
// every configuration. It is NOT here to make several samples of one pixel
// safe within one dispatch: that configuration is exactly the film hazard
// spec section 4.5 records, and this subsystem's resolution is that it never
// occurs -- ONE SAMPLE PER PIXEL PER DISPATCH. Float addition is not
// associative, so several paths of one pixel adding into these three floats
// inside one dispatch would make the result depend on a scheduling order the
// seed does not control, and a PRB forward/backward comparison would see it
// as a gradient that is almost right and irreproducible between runs. The
// full argument, the two alternatives rejected and the three places the
// choice is enforced are on `diffVertexHook` in shaders/diff/wf_scatter.comp,
// which is the instantiation that owns this write.
//
// This buffer is CALLER-OWNED -- it is not part of WavefrontBuffers -- and it
// is written by every FORWARD scatter dispatch. It therefore MUST be passed
// to WavefrontLoop::record's `extraBarrierBuffers`, which is what folds it
// into the loop's post-dispatch COMPUTE -> COMPUTE barrier. See the film note
// on the forward hook.
//
// WHAT THIS BUFFER DOES NOT CONTAIN (informational, review Finding 3 on
// Stage 0b-2b Task 5 -- read this before comparing it against a PathTracer
// image in Task 6). It holds ONLY MIS direct lighting at surface vertices:
//   * NO escape/environment term. wf_intersect.comp compacts only survivors
//     of the trace, so a path that misses everything and escapes to the
//     environment is simply dropped from the next bounce's queue -- it never
//     reaches this stage again and contributes nothing further here.
//   * NO emissive-surface term. Nothing in this pipeline evaluates one yet;
//     a hit surface's own emission (if it had any) is not part of the film
//     contribution the forward hook forms.
// A PathTracer parity comparison will therefore differ from this film by
// exactly the directly-visible environment plus any emissive-surface term --
// neither exists on the PathTracer side of that comparison either today, but
// closing the box here (this file's own test rig) makes the gap
// unobservable from inside this subsystem. It is not a defect in this task;
// it is the shape of what "MIS-combined radiance" was asked to accumulate.
layout(std430, binding = 9) buffer Film {
    float v[];
} film;

layout(push_constant) uniform Push {
    uint capacity;
    uint srcQueueBase;
    uint srcCountSlot;
    uint dstQueueBase;
    uint dstCountSlot;
    // Base colour, as a grey scalar. There is one material for the whole
    // dispatch: nothing in the wavefront path state carries a per-hit
    // material id yet, and inventing one before there is a scene that needs
    // it would be untestable plumbing.
    float albedo;
    uint iterationSeed;
    // Material, matching WavefrontLoop::ScatterPush's tail. roughness and
    // metallic go through pbr_unpack.glsl's unpackHitPbr below -- the SAME
    // unpack the RT closest-hit path uses -- so the two pipelines' material
    // interpretation (including its 0.01 roughness floor) cannot drift.
    // specularWeight = 0 with metallic = 0 is the pure-Lambertian
    // configuration whose per-bounce weight is exactly `albedo`.
    float roughness;
    float metallic;
    float specularWeight;
    // --- Environment (Stage 0b-2b Task 3), matching ScatterPush's tail.
    // envWidth/envHeight are the equirectangular CDF's dimensions: the
    // marginal buffer holds envHeight floats and the conditional
    // envWidth*envHeight. Either being 0 means "no environment configured"
    // and is the one case sampleEnvMap must not be called for -- its binary
    // searches would index a zero-length row. envIntegral is passed through
    // for the header's signature and for Task 4; sampleEnvMap's body does
    // not currently read it (pdfEnvMap does not even take it), because the
    // solid-angle pdf is recovered from CDF differences, which are already
    // normalised.
    uint envWidth;
    uint envHeight;
    float envIntegral;
    // --- Film (Stage 0b-2b Task 5), matching ScatterPush's tail. The
    // number of PIXELS the binding-9 film buffer holds; it is 3*this many
    // floats. 0 means "no film configured" and disables the accumulation
    // entirely. The bounds guard below checks pixelIndex against THIS
    // number -- unlike capacity, it is a property of a CALLER-OWNED
    // allocation the shader has no other way to learn the size of. That
    // guard rejects a pixelIndex this value says is out of range; it does
    // NOT protect against this value itself being wrong. A caller that
    // passes a filmPixelCount larger than the buffer it actually bound
    // still gets a write past the end of that allocation -- keeping this
    // number and the buffer's real size in agreement is the caller's own
    // invariant, not something this guard verifies.
    uint filmPixelCount;
} pc;

// Fixed, non-dynamic draw count per bounce: this shader and the CPU probe
// replaying ohao::diff::PathRng must agree on this exactly. Kept as a literal
// constant (not derived from anything) so the CPU side can predict it without
// querying the GPU for anything but the final draw values/count.
//
// FIVE. Three for the BSDF -- a 2-D sample for the direction plus a 1-D
// sample to choose between the diffuse and specular lobes -- and two more
// for the environment sample. The count must stay INDEPENDENT of which lobe
// is chosen and of whether the path hit anything: a data-dependent draw
// count would make the fast-forward above unpredictable, and path-replay
// backpropagation would walk a different stream than the forward pass --
// which is why uLobe is drawn unconditionally even in the configurations
// where the lobe choice is foregone, and why the two environment draws are
// taken before the miss guard rather than inside the hit branch.
//
// It was 2 in Stage 0b-1 (a placeholder direction sampler) and 3 in Stage
// 0b-2b Task 2 (the real BSDF). The environment draws are appended AFTER
// the BSDF's three, not interleaved, so every earlier stream position keeps
// its meaning and diff_gpu_probe.cpp's per-bounce RNG-parity checks compare
// the same (u1, u2) they always did -- only the per-bounce stride moved.
const uint kDrawsPerBounce = 5u;

// Floats per PATH INDEX in the binding-7 sink. Must equal
// ohao::diff::kNeeSampleFloats (tests/diff/gpu_probe_context.hpp), which is
// what sizes the buffer and strides the readback; a mismatch is a silent
// wrong-slot read, not a validation error.
//
// Naming each other in a comment was the whole of the tie and was not
// enough. GLSL has no static_assert and the value is not reflected out of
// the SPV under a name, so diff_gpu_probe.cpp now READS THIS LINE from the
// shader source at startup and refuses to run if the two disagree. The
// literal below is parsed by the regular expression
// `const uint kNeeSampleFloats = ([0-9]+)u;` -- keep the spelling on one
// line and in this form.
const uint kNeeSampleFloats = 25u;

// How far a shadow ray may travel before the environment is considered
// reached. The environment is at infinity, so this only has to exceed the
// longest chord of any scene this stage is run against; 1000 is
// wf_intersect.comp's own trace tMax, and using the same number keeps "this
// ray found geometry" meaning the same thing in both stages.
const float kShadowTMax = 1000.0;

// Offset of the shadow ray's origin along the geometric normal. The SAME
// 1e-4 the path-continuation offset below uses, and for the same reason: a
// ray leaving a face must not re-hit the face it left. Deriving both from
// one constant is what stops a future change to one of them from silently
// desynchronising the visibility a light sample sees from the geometry the
// next bounce sees.
const float kSurfaceOffset = 1e-4;

// ===========================================================================
// THE PER-VERTEX HOOK -- the ONE thing an instantiation supplies
// ===========================================================================
//
// Everything a path-replay-backpropagation vertex needs, in ONE struct
// passed `inout`. The hook's signature is FIXED HERE and is what Stage 1's
// remaining tasks are handed verbatim; extending what a vertex carries is a
// new FIELD, never a new parameter, so neither instantiation's call site --
// nor any future one -- ever has to change again.
//
// WHY THESE FIELDS. Spec section 6.1 states the PRB recursion at a vertex as
// two lines:
//
//     dL/dtheta  +=  dL * (df/dtheta) * L_i / pdf     // scatter into arena
//     dL_next     =  dL * f / pdf                     // propagate onward
//
// THE SECOND LINE IS THIS INTEGRATOR'S VERBATIM: `bsdfWeight` is bit-exactly
// what the traversal multiplied throughput by, so `dL_next = dL * bsdfWeight`
// is the same arithmetic the forward pass did. THE FIRST LINE IS NOT, and
// reading it as though it were is how a wrong gradient is derived from a
// right-looking formula. The spec writes the scatter line for a
// SINGLE-STRATEGY estimator, in which `L_i` is the INCIDENT radiance arriving
// along the one sampled direction, `f` is evaluated at that direction and
// `pdf` is the one density it was drawn from. What this traversal forms at a
// vertex is not that. It is a TWO-STRATEGY MIS combination of the
// direct-lighting integral -- next event (a light sample) and BSDF sampling
// -- and each strategy carries its OWN direction, its own f, its own density,
// its own visibility and its own MIS weight. There is no single (f, L_i, pdf)
// triple at which the spec's line could be evaluated.
//
// Expanding the spec's scatter line over the strategies this estimator
// actually uses gives the line every consumer of this struct must use:
//
//     dL/dtheta += dL * SUM_s [ w_s * (df_s/dtheta) * cos_s * L_s * V_s / p_s ]
//
//   s = E (next event)  : dir = envDir, cos_E = max(dot(normal, envDir), 0),
//                         L_E = envRadiance, V_E = visEnv, p_E = envPdf,
//                         w_E = wEnv
//   s = B (BSDF sample) : dir = bsdfDir, cos_B is the cosine ALREADY FOLDED
//                         INTO `f` (which is f*cos at bsdfDir, so the product
//                         (df_B/dtheta)*cos_B is the derivative of `f`
//                         itself), L_B = bsdfRadiance, V_B = visBsdf,
//                         p_B = pdf, w_B = wBsdf
//
// Every factor of that is a field below. `df_s/dtheta` is the only thing a
// consumer still has to compute, and it is computed at
// (normal, wo, dir_s, material) -- all fields too.
//
// WHAT THIS STRUCT MUST NOT BE READ AS. `Lr` below is the MIS-COMBINED
// REFLECTED DIRECT RADIANCE -- SUM_s w_s * f_s*cos_s*L_s*V_s/p_s. It already
// contains f and it has already been divided by the densities. It is L_r, NOT
// the spec's L_i. Substituting it into the spec's scatter line double-counts
// f and squares the pdf. This field was called `Li` for exactly one task and
// the rename is the fix, because a field named for incident radiance that
// holds reflected radiance is the whole of the defect. The proof that it is
// L_r is one paragraph down and was always here: `throughput * Lr` is EXACTLY
// the forward film contribution, and if `Lr` held incident radiance that
// product would be missing f/pdf.
//
// Read off what appears in those lines and this struct's core is forced:
//
//   * `adjoint` is `dL` -- IN and OUT. The hook reads the adjoint arriving
//     at this vertex and writes the propagated `dL_next` back into the same
//     field, which is why the whole struct is `inout` and why the adjoint is
//     a member rather than a return value: a return value could carry the
//     propagated adjoint but not the incoming one. WHERE it comes from and
//     where it goes is the INSTANTIATION's business, not this file's -- a
//     later task adds a path-state field for it, and that change touches
//     neither this signature nor the traversal.
//   * `f`, `pdf` and `bsdfWeight` are the same quantity written three ways,
//     and all three are here on purpose. `bsdfWeight` is what the traversal
//     actually multiplies throughput by (f*cos/pdf, i.e. the `f / pdf` of
//     the recursion's second line); `f` is f*cos alone; `pdf` is the density
//     the direction was drawn with.
//
//     THE REASON ALL THREE ARE HERE IS NOT that offering one and making the
//     others reconstructable would cost a rounding difference. An earlier
//     version of this comment said that, and it was self-undercutting: `f`
//     is DERIVED here, as `weight * pdf` (see the fill site below), and so
//     carries exactly the round-trip rounding the claim said keeping all
//     three avoided. The true reason is plainer, and holds. The scatter line
//     wants f*cos and the density SEPARATELY while the propagate line wants
//     the RATIO; keeping all three spares every consumer a derivation it
//     would otherwise repeat, and the derived one is bit-reproducible
//     because it is derived ONCE, in this shared source, so both
//     instantiations compile the identical expression and get identical
//     bits. That is the property the replay rests on, and it is a property
//     of where the derivation lives, not of how many fields there are.
//
//     What must NOT be re-derived is `bsdfWeight`. The path's throughput was
//     multiplied by that exact value, so `dL_next = dL * bsdfWeight` is
//     bit-identical to the forward decay while `dL * f / pdf` would not be.
//   * `Lr` is the MIS-combined REFLECTED direct radiance at this vertex --
//     SUM_s w_s * f_s*cos_s*L_s*V_s/p_s, with f and the division by the
//     densities already inside it. Multiplied by `throughput` it is exactly
//     the forward pass's film contribution, which is why the FORWARD
//     instantiation's hook is nothing but `film += throughput * Lr`. It is
//     NOT the spec's `L_i` and it does not belong in the scatter line -- see
//     "WHAT THIS STRUCT MUST NOT BE READ AS" above. It is a field because
//     the forward hook needs it and because a consumer wanting the vertex's
//     whole direct term should read the one the traversal formed rather than
//     re-summing the per-strategy fields in a second place that could drift.
//   * The PER-STRATEGY inputs the scatter line is summed over: `envDir`,
//     `envPdf`, `envRadiance`, `visEnv` and `wEnv` for next event;
//     `bsdfRadiance`, `visBsdf` and `wBsdf` for BSDF sampling (whose
//     direction, f*cos and density are `bsdfDir`, `f` and `pdf` above).
//     These were absent for one task, during which this comment claimed
//     "every factor those two lines need is already a field" -- which was
//     false: without them the next-event half of the scatter line cannot be
//     written at all. They are FIELD additions, which is the signature's
//     central virtue holding rather than failing: extending what a vertex
//     carries has still never changed the hook's parameter list or either
//     instantiation's call site.
//   * `throughput` is the path throughput ON ARRIVAL, before this vertex's
//     `bsdfWeight` decay. The estimator in `Lr` already contains this
//     vertex's f*cos/p; multiplying by the post-decay value would count this
//     bounce's BSDF twice.
//   * `pathIndex`, `pixelIndex`, `capacity` and `bounce` are how a hook
//     addresses itself: an arena scatter has to know which path and which
//     vertex it is writing for, and `capacity` is what every path-state
//     accessor is indexed by.
//   * The geometry and material fields (`origin`, `dir`, `hitT`, `position`,
//     `normal`, `wo`, `wi`, `bsdfDir`, `baseColor`, `roughness`, `metallic`,
//     `specularWeight`) are the arguments `df/dtheta` will be evaluated at.
//     They are the traversal's own values, not recomputed ones: a hook that
//     re-derived the shading frame from path state would be a second
//     implementation of the thing this file exists to have exactly one of.
//
//     THEY ARE NOT ALL IN THE SAME SPACE, and a chain rule taken without
//     noticing that is wrong by a factor nothing would flag. `roughness` and
//     `metallic` are POST-`unpackHitPbr` -- floor included, so the 0.01
//     roughness clamp has already been applied and d(roughness)/d(the pushed
//     roughness) is 1 where the floor did not engage and 0 where it did.
//     `baseColor` and `specularWeight` are the RAW pushed values with no
//     unpacking between. So that a consumer never has to INFER which side of
//     the floor it is on, `rawRoughness` and `rawMetallic` carry the pushed
//     values alongside the unpacked ones: the floor engaged exactly when
//     `roughness != rawRoughness`, which makes the chain rule from a
//     parameter to the unpacked value decidable from the fields instead of
//     reconstructed from a constant copied out of bsdf.glsl.
//   * `hit` is false when the miss guard was taken. Every field below the
//     identity block is then zero, and a hook must not read the normal --
//     the stored one is UNDEFINED on a miss (see psGetNormal).
//
// `wi` and `bsdfDir` are BOTH here and are not always equal: when the GGX
// VNDF draws a below-horizon direction the traversal keeps the zero weight
// (the correct Monte Carlo answer) but substitutes a cosine-hemisphere
// direction for path continuation. `bsdfDir` is what the BSDF was evaluated
// at and is therefore what a derivative must be taken at; `wi` is where the
// path actually went. Collapsing them into one field is the kind of thing
// that reads as harmless and silently differentiates the wrong direction.
//
// `pdf` IS THE MIXTURE DENSITY, NOT A LOBE-CONDITIONAL ONE, and a consumer
// deriving df/dtheta needs to know that before it starts. diffBsdfSample
// returns the pdf that diffBsdfEval computes (shaders/includes/diff/bsdf.glsl)
// -- the diffuse and specular lobes' densities combined by their selection
// probabilities -- not the density of whichever lobe `uLobe` happened to
// select. `f` and `pdf` are therefore PURE FUNCTIONS of
// (normal, wo, bsdfDir, material): they do not depend on the lobe choice at
// all. A consumer differentiating them needs neither `uLobe` nor a lobe
// index, which is why neither is a field, and why a future task must not add
// one on the assumption that it does.
//
// WHAT A HOOK MAY AND MAY NOT DO -- the contract, stated because nothing
// enforces it structurally.
//
// The hook is a function body inside this translation unit, so EVERYTHING
// this file declares is in scope: the Push block, every binding, and every
// path-state accessor. The compiler will not stop a hook from calling
// psSetOrigin/psSetDir/psSetThroughput/psSetBounce, or from writing `queues`
// or `counters`. A hook that did would diverge the NEXT bounce of ONE
// instantiation while leaving this bounce's trace record identical -- exactly
// the failure this task exists to make impossible -- and no source-level
// check sees it: the instantiation tie constrains what a `.comp` file
// DECLARES, not what its hook body CALLS, and the replay-equivalence check
// catches it only at bounce b+1, and only if a bounce b+1 is run. So it is a
// contract, written in the one place both instantiations read:
//
//   A HOOK MUST NOT WRITE PATH STATE, THE QUEUES, OR THE COUNTERS. Everything
//   that advances the path is done by this file, below, identically for both
//   instantiations. A hook that writes any of them has made the two
//   instantiations walk different paths, which is the one thing they may not
//   do.
//
//   A HOOK MAY WRITE ITS OWN OUTPUT SINK, and nothing else. The forward hook
//   writes the film (binding 9); a gradient hook writes the gradient arena.
//   It may READ anything. It may write `v.adjoint` -- that is what the
//   `inout` is for -- and any other field of `v`, which dies at the return.
//
//   The probe binds each instantiation its OWN film buffer precisely so that
//   a hook which breaks the first half of this contract cannot contaminate
//   the record the other instantiation is compared against.
//
// The hook is called ONCE per invocation, at the point the forward pass used
// to accumulate the film: after the vertex's estimators exist and after path
// state has been advanced, before the bounce counter is incremented and the
// path re-queued.
struct DiffVertex {
    // --- Identity -----------------------------------------------------
    uint pathIndex;
    uint pixelIndex;
    uint capacity;
    uint bounce;   // the bounce index of THIS vertex, pre-increment
    bool hit;      // false => the miss guard was taken; see above

    // --- Geometry, exactly as the traversal read or computed it --------
    vec3 origin;    // ray origin that produced this vertex
    vec3 dir;       // ray direction that produced this vertex
    float hitT;     // -1 on a miss
    vec3 position;  // origin + dir * hitT
    vec3 normal;    // forward-facing geometric normal (undefined if !hit)
    vec3 wo;        // -dir, pointing away from the surface
    vec3 wi;        // the direction the path CONTINUES along
    vec3 bsdfDir;   // the direction the BSDF actually DREW

    // --- The PRB recursion's factors (spec 6.1, corrected above) -------
    vec3 f;           // f * cos(theta) at bsdfDir, derived as weight*pdf
    float pdf;        // the MIXTURE density bsdfDir was drawn with
    vec3 bsdfWeight;  // f*cos/pdf -- what throughput was multiplied by
    vec3 Lr;          // MIS-combined REFLECTED direct radiance -- NOT L_i
    vec3 throughput;  // path throughput ON ARRIVAL, before the decay

    // --- The direct-lighting estimator's TWO STRATEGIES ----------------
    // What the corrected scatter line is summed over, one group per
    // strategy. The BSDF strategy's direction, f*cos and density are
    // `bsdfDir`, `f` and `pdf` above and are not repeated here. All of
    // these are zero on the miss path, and the next-event group is zero
    // for an unconfigured environment, where that strategy contributes
    // nothing.
    vec3 envDir;         // s=E: the light sample's direction
    float envPdf;        // s=E: the density it was drawn from (0 => no env)
    float envRadiance;   // s=E: L at envDir (grey)
    float visEnv;        // s=E: shadow-ray visibility, exactly 1 or 0
    float wEnv;          // s=E: the MIS weight this strategy carries
    float bsdfRadiance;  // s=B: L at bsdfDir (grey)
    float visBsdf;       // s=B: shadow-ray visibility, exactly 1 or 0
    float wBsdf;         // s=B: the MIS weight this strategy carries

    // --- Material. NOTE THE TWO SPACES -- see the note above. ----------
    vec3 baseColor;        // RAW pushed value
    float roughness;       // POST-unpackHitPbr: the 0.01 floor is applied
    float metallic;        // POST-unpackHitPbr
    float specularWeight;  // RAW pushed value
    float rawRoughness;    // what unpackHitPbr was GIVEN
    float rawMetallic;     // what unpackHitPbr was GIVEN

    // --- THE ADJOINT. Read by the hook, written by the hook. -----------
    vec3 adjoint;
};

/// THE HOOK. Declared here, DEFINED BY THE INCLUDER. This prototype is what
/// fixes the signature: a `.comp` that includes this file and does not define
/// this exact function fails at glslc time, which is the failure mode worth
/// having -- a hook that silently was not called would produce a forward pass
/// with no film and a backward pass with no gradient, and both of those read
/// as a physics bug rather than as a wiring one.
void diffVertexHook(inout DiffVertex v);

/// Floats per PATH INDEX in the binding-3 VERTEX TRACE record (the sink this
/// file writes at `debugDraws.v[pathIndex * 18u + <slot>]`).
///
/// It was 3 through Stage 0b-2b -- (u1, u2, drawCount) -- and slots 0, 1 and
/// 2 still carry exactly those three values, so the per-bounce RNG-parity
/// checks that have read them since Stage 0b-1 are untouched. The fifteen
/// slots appended after them are what makes REPLAY EQUIVALENCE observable:
/// all five of the bounce's draws (not just the first two), and the origin,
/// direction, throughput and hit distance the traversal read out of path
/// state before it overwrote them. Two instantiations that walk the same
/// path produce bit-identical records here; one that consumes a single extra
/// RNG value does not.
///
/// Must equal `ohao::diff::kDebugDrawFloats` (tests/diff/gpu_probe_context.hpp),
/// which sizes the buffer and strides the readback, and the per-slot meanings
/// must match its `TraceSlot` enum. Both halves of that are TIED, not merely
/// documented: `diff_gpu_probe.cpp`'s `checkWfScatterSinkLayoutTie()` parses
/// the write statements below out of THIS SOURCE and refuses to run the probe
/// unless every slot is written exactly once, at the offset `TraceSlot` names,
/// carrying the expression that enumerator documents. The literal is spelled
/// inline in those statements (`pathIndex * 18u + <N>u`) rather than through
/// this constant because that is the spelling the tie's regex reads; this
/// declaration exists so the number has a name and a place to be explained.
const uint kTraceFloats = 18u;

/// Unoccluded-ness of `dir` from `origin`: 1.0 if the ray reaches
/// kShadowTMax without hitting anything, 0.0 otherwise.
///
/// The inline ray query is wf_intersect.comp's, minus the hit attributes:
/// gl_RayFlagsTerminateOnFirstHitEXT because any hit at all settles the
/// question, and gl_RayFlagsOpaqueEXT because nothing in this subsystem
/// builds non-opaque geometry. tMin is 0 rather than a small epsilon --
/// self-intersection is handled by offsetting the ORIGIN off the surface
/// (kSurfaceOffset above), which is the same choice wf_intersect.comp makes
/// and the reason both stages can be reasoned about the same way.
///
/// ONE function, two call sites (the light sample's direction and the BSDF
/// sample's), for the reason nee.glsl's header gives at length.
float diffShadowVisibility(vec3 origin, vec3 dir) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, topLevelAS,
                          gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin,
                          0.0, dir, kShadowTMax);
    while (rayQueryProceedEXT(rq)) { }
    return (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
               ? 1.0
               : 0.0;
}

/// The traversal. Called from each instantiation's `main`, and the ONLY
/// thing either of them calls.
void diffTraverse() {
    const uint slot = gl_GlobalInvocationID.x;
    const uint srcCount = min(counters.value[pc.srcCountSlot], pc.capacity);
    if (slot >= srcCount) return;

    const uint pathIndex = queues.idx[pc.srcQueueBase + slot];

    const uint pixelIndex = psGetPixelIndex(pathIndex, pc.capacity);
    const uint sampleIndex = psGetSampleIndex(pathIndex, pc.capacity);
    const uint bounce = psGetBounce(pathIndex, pc.capacity);

    // Reconstruct from the tuple -- never from a register carried across a
    // dispatch boundary -- then fast-forward past every earlier bounce's
    // draws before taking this bounce's own. This loop is the seed
    // invariant made literal: it is what makes this dispatch produce the
    // same stream a megakernel's register-resident RNG would have, despite
    // running as an entirely separate command.
    DiffPathRng rng = diffRngForPath(pixelIndex, sampleIndex, pc.iterationSeed);
    for (uint i = 0u; i < bounce * kDrawsPerBounce; ++i) {
        diffRngNext1D(rng);
    }
    // Direction sample FIRST, lobe choice second. That order is deliberate:
    // it keeps the two values the debug sink records identical to the ones
    // the Stage 0b-1 placeholder recorded, so the per-bounce RNG-parity
    // checks compare the same stream positions they always did and only the
    // draw COUNT moved.
    const float u1 = diffRngNext1D(rng);
    const float u2 = diffRngNext1D(rng);
    const float uLobe = diffRngNext1D(rng);
    const float uEnv1 = diffRngNext1D(rng);
    const float uEnv2 = diffRngNext1D(rng);

    // --- Environment importance sample. Observed, not consumed (see the
    // file header). Guarded on a configured environment: with envWidth or
    // envHeight 0 there is no CDF to search, and the (0,0,0) direction with
    // pdf 0 written instead is the "no environment" sentinel a consumer has
    // to test before dividing by it.
    vec3 envDir = vec3(0.0);
    float envPdf = 0.0;
    if (pc.envWidth > 0u && pc.envHeight > 0u) {
        sampleEnvMap(uEnv1, uEnv2, pc.envWidth, pc.envHeight, pc.envIntegral, envDir, envPdf);
    }
    envSamples.v[pathIndex * 4u + 0u] = envDir.x;
    envSamples.v[pathIndex * 4u + 1u] = envDir.y;
    envSamples.v[pathIndex * 4u + 2u] = envDir.z;
    envSamples.v[pathIndex * 4u + 3u] = envPdf;

    const vec3 origin = psGetOrigin(pathIndex, pc.capacity);
    const vec3 dir = psGetDir(pathIndex, pc.capacity);
    const float hitT = psGetHitT(pathIndex, pc.capacity);
    // The path's throughput as it ARRIVES here, read once, before anything
    // below can overwrite it. The hit branch decays THIS value rather than
    // re-reading the field, so the trace record and the decay cannot
    // disagree about what arrived.
    const vec3 pathThroughput = psGetThroughput(pathIndex, pc.capacity);

    // --- THE VERTEX TRACE RECORD (binding 3, kTraceFloats floats per path).
    //
    // Written HERE, after the draws and after the reads and BEFORE the miss
    // guard, for three reasons that are each load-bearing:
    //
    //  * BEFORE the guard, so a missed path produces a record too -- the
    //    same reason the draws themselves are taken before it. A sink
    //    written only on the hit path leaves the previous dispatch's values
    //    at this path's offset and the host cannot tell stale from fresh.
    //  * AFTER the origin/dir reads and BEFORE psSetOrigin/psSetDir, so the
    //    recorded ray is the one that PRODUCED this vertex, not the one the
    //    path continues along. Recording the post-advance values would make
    //    a replay that advanced the path differently still look identical
    //    at every bounce but the last.
    //  * Every value here comes from path state, or from the RNG rebuilt out
    //    of (pixelIndex, sampleIndex, iterationSeed, bounce). Nothing is
    //    handed in. That is what makes a comparison of two instantiations'
    //    records a statement about the traversal rather than about the
    //    plumbing that ran it.
    //
    // Slot meanings are TIED to ohao::diff::TraceSlot -- see kTraceFloats.
    debugDraws.v[pathIndex * 18u +  0u] = u1;
    debugDraws.v[pathIndex * 18u +  1u] = u2;
    debugDraws.v[pathIndex * 18u +  2u] = uintBitsToFloat(rng.draws);
    debugDraws.v[pathIndex * 18u +  3u] = uLobe;
    debugDraws.v[pathIndex * 18u +  4u] = uEnv1;
    debugDraws.v[pathIndex * 18u +  5u] = uEnv2;
    debugDraws.v[pathIndex * 18u +  6u] = origin.x;
    debugDraws.v[pathIndex * 18u +  7u] = origin.y;
    debugDraws.v[pathIndex * 18u +  8u] = origin.z;
    debugDraws.v[pathIndex * 18u +  9u] = dir.x;
    debugDraws.v[pathIndex * 18u + 10u] = dir.y;
    debugDraws.v[pathIndex * 18u + 11u] = dir.z;
    debugDraws.v[pathIndex * 18u + 12u] = pathThroughput.x;
    debugDraws.v[pathIndex * 18u + 13u] = pathThroughput.y;
    debugDraws.v[pathIndex * 18u + 14u] = pathThroughput.z;
    debugDraws.v[pathIndex * 18u + 15u] = hitT;
    debugDraws.v[pathIndex * 18u + 16u] = uintBitsToFloat(bounce);
    debugDraws.v[pathIndex * 18u + 17u] = uintBitsToFloat(pixelIndex);

    // The vertex handed to the hook. Declared here so the miss path fills in
    // an all-zero one with `hit` false, rather than the hook being called
    // only on the hit branch: "every invocation calls the hook exactly once"
    // is the invariant a replay's draw-count arithmetic and a future
    // adjoint's per-bounce bookkeeping are both stated against.
    DiffVertex vtx;
    vtx.pathIndex = pathIndex;
    vtx.pixelIndex = pixelIndex;
    vtx.capacity = pc.capacity;
    vtx.bounce = bounce;
    vtx.hit = (hitT >= 0.0);
    vtx.origin = origin;
    vtx.dir = dir;
    vtx.hitT = hitT;
    vtx.position = vec3(0.0);
    vtx.normal = vec3(0.0);
    vtx.wo = -dir;
    vtx.wi = vec3(0.0);
    vtx.bsdfDir = vec3(0.0);
    vtx.f = vec3(0.0);
    vtx.pdf = 0.0;
    vtx.bsdfWeight = vec3(0.0);
    vtx.Lr = vec3(0.0);
    vtx.throughput = vec3(0.0);
    // The per-strategy group, zero until the hit branch fills it. A miss
    // takes the environment draws (the stream must not depend on the
    // branch) but forms no estimator at all, so there is no strategy to
    // describe and the whole group stays zero -- the same "every field
    // below the identity block is zero on a miss" rule the rest obeys.
    vtx.envDir = vec3(0.0);
    vtx.envPdf = 0.0;
    vtx.envRadiance = 0.0;
    vtx.visEnv = 0.0;
    vtx.wEnv = 0.0;
    vtx.bsdfRadiance = 0.0;
    vtx.visBsdf = 0.0;
    vtx.wBsdf = 0.0;
    // baseColor, specularWeight and the two raw PBR values are RAW pushed
    // values -- there is no unpacking between the Push block and the field,
    // so they are as valid on the miss path as anywhere and are set here
    // rather than in the hit branch. `roughness`/`metallic` are the
    // unpacked ones and are not: unpackHitPbr runs on the hit path only.
    vtx.baseColor = vec3(pc.albedo);
    vtx.roughness = 0.0;
    vtx.metallic = 0.0;
    vtx.specularWeight = pc.specularWeight;
    vtx.rawRoughness = pc.roughness;
    vtx.rawMetallic = pc.metallic;
    // No adjoint exists anywhere in this subsystem yet (Stage 1 Task 1
    // deliberately adds none), so it starts at zero. A later task sources it
    // from path state; that change is confined to this one line.
    vtx.adjoint = vec3(0.0);

    // --- The two MIS strategies' per-sample record, declared HERE so that
    // the single write at the end of this function covers the miss path too
    // (all zeros, status 0). A sink written only on the hit path would keep
    // whatever the previous dispatch left at this path's offset, and the
    // host cannot tell a stale value from a fresh one.
    DiffMisTerm neeTerm;
    neeTerm.unweighted = vec3(0.0);
    neeTerm.wOwn = 0.0;
    neeTerm.wOther = 0.0;
    DiffMisTerm bsdfTerm = neeTerm;
    float envRadiance = 0.0;
    float bsdfRadiance = 0.0;
    float pdfEnvAtBsdfDir = 0.0;
    float pdfEnvTexelAtBsdfDir = 0.0;
    float pdfBsdfAtEnvDir = 0.0;
    float visEnv = 0.0;
    float visBsdf = 0.0;
    float surfaceBranch = 0.0;
    vec3 bsdfSampleDir = vec3(0.0);
    float bsdfSamplePdf = 0.0;
    // The path's throughput ON ARRIVAL at this vertex -- i.e. BEFORE this
    // bounce's f*cos/pdf decay. Declared out here, and recorded to the sink
    // unconditionally like everything else, so the host can reconstruct the
    // exact film contribution this dispatch computed from independently
    // recorded primitives. Zero on the miss path, where there is no vertex.
    vec3 arrivalThroughput = vec3(0.0);

    // MISS GUARD. The stored Normal is UNDEFINED on a miss -- wf_intersect.comp
    // deliberately leaves PS_NORMAL_X/Y/Z untouched when HitT is -1, so they
    // hold whatever the arena held before, not a sentinel (see psGetNormal in
    // path_state.glsl and PathStateField in path_state_layout.hpp). There is
    // also no hit point to move to and no surface to scatter off. Nothing in
    // the current pipeline routes a missed path here -- wf_intersect.comp
    // compacts only survivors into the ring this stage consumes -- but the
    // guard is not conditional on that continuing to hold. It re-queues
    // without touching throughput, origin, dir or normal, preserving this
    // stage's "every invocation re-queues" invariant while refusing to read
    // an undefined field. The RNG draws above are taken BEFORE this point so
    // that a missed path still advances its stream by exactly
    // kDrawsPerBounce, keeping the fast-forward arithmetic uniform.
    if (hitT >= 0.0) {
        // The real geometric normal of the hit, written into path state by
        // wf_intersect.comp.
        vec3 normal = psGetNormal(pathIndex, pc.capacity);

        // Re-establish "forward-facing" AT THE POINT OF USE. wf_intersect.comp
        // guarantees dot(Normal, Dir) <= 0 for the Dir that produced the hit,
        // so in the real pipeline -- where intersect runs immediately before
        // scatter, every bounce -- this flip never fires and costs one dot
        // product. It exists because that guarantee is about a PAIR of
        // fields, and scatter is what breaks the pair: it overwrites Dir with
        // the scattered direction while leaving Normal alone, so any caller
        // that runs scatter again without an intervening intersect (as
        // diff_gpu_probe.cpp's check 14 deliberately does -- intersect once,
        // scatter four times) hands this stage a Normal and a Dir from
        // different bounces. Without the flip the BSDF would see
        // dot(N, V) < 0, correctly report zero throughput for a direction
        // below the surface, and the path would go black for a reason that
        // has nothing to do with the BSDF being wrong. A one-sided opaque
        // BSDF has no other sensible reading of a normal on the far side of
        // the surface from the viewer.
        //
        // This does NOT weaken the geometric-normal check: check 19 asserts
        // the STORED normal against an analytic oracle directly, so an
        // intersect stage that forgot to flip is caught there, not hidden
        // here.
        if (dot(normal, dir) > 0.0) {
            normal = -normal;
        }

        // V points away from the surface, back along the incoming ray.
        const vec3 V = -dir;

        float roughness;
        float metallic;
        unpackHitPbr(vec3(pc.roughness, pc.metallic, 0.0), roughness, metallic);

        vec3 newDir;
        vec3 weight;
        float pdf;
        diffBsdfSample(normal, V, vec3(pc.albedo), roughness, metallic, pc.specularWeight,
                       vec2(u1, u2), uLobe, newDir, weight, pdf);

        // PATH CONTINUATION for a rejected sample. diffBsdfSample returns
        // the direction it actually drew, which for the GGX VNDF's
        // below-horizon tail is a direction the BRDF is zero at: pdf = 0 and
        // weight = 0. Zero weight is the correct Monte Carlo answer and is
        // kept -- the path from here on contributes nothing -- but the
        // direction cannot be: tracing a ray that points INTO the surface
        // would send it out through the geometry, and the fused-loop
        // survival theorem (every path alive at every bounce) needs
        // dot(newDir, normal) > 0 for every path regardless of what the BSDF
        // decided. Substituting the cosine-hemisphere direction the same
        // uDir would have produced keeps the ray inside the scene at zero
        // cost in extra random draws and, because the weight is already
        // exactly zero, introduces no bias.
        //
        // Captured BEFORE the substitution below: the direct-lighting
        // estimator has to be evaluated at the direction the BSDF actually
        // DREW and at the density it was drawn with, not at a replacement
        // chosen for path continuation. When the sample was rejected both
        // are zero, so the contribution is zero either way -- but making
        // that hold by construction rather than by coincidence is what keeps
        // the estimator's expectation equal to the integral.
        const vec3 bsdfDir = newDir;
        bsdfSampleDir = bsdfDir;
        bsdfSamplePdf = pdf;

        if (pdf <= 0.0) {
            newDir = diffCosineHemisphere(normal, u1, u2);
        }

        // --- NEXT-EVENT ESTIMATION AND MIS -----------------------------
        //
        // Both strategies estimate the SAME direct-lighting integral at this
        // shading point, and both go through nee.glsl's ONE diffMisTerm.
        // Neither touches path state: their results are recorded per sample
        // and accumulated on the HOST (see the file header).
        const vec3 hitPoint = origin + dir * hitT;
        const vec3 shadowOrigin = hitPoint + normal * kSurfaceOffset;
        surfaceBranch = 1.0;

        // Strategy E -- next event. The light sample is `envDir`/`envPdf`
        // drawn ABOVE, the very pair binding 6 records. Not re-drawn here.
        vec3 fEnv;
        diffBsdfEval(normal, V, envDir, vec3(pc.albedo), roughness, metallic, pc.specularWeight,
                     fEnv, pdfBsdfAtEnvDir);
        const float nDotLEnv = max(dot(normal, envDir), 0.0);
        if (envPdf > 0.0 && nDotLEnv > 0.0) {
            visEnv = diffShadowVisibility(shadowOrigin, envDir);
        }
        envRadiance = diffEnvRadianceFromPdf(envPdf, pc.envWidth, pc.envHeight, pc.envIntegral);
        neeTerm = diffMisTerm(fEnv * nDotLEnv, vec3(envRadiance), visEnv, envPdf, pdfBsdfAtEnvDir);

        // Strategy B -- BSDF sampling. pdfEnvMap is the other half of
        // env_sampling.glsl's pair and, until this call, had no caller under
        // test anywhere in this repository: check 24 deliberately writes its
        // own inverse rather than using it. It is what makes the BSDF
        // strategy's MIS weight the balance heuristic's actual partner
        // density and not a guess.
        //
        // f*cos is reconstructed as weight*pdf rather than re-evaluated:
        // diffBsdfSample already returns weight = f*cos/pdf, and multiplying
        // it back is one instruction against a second full BSDF evaluation.
        // The round trip is not bit-exact (the pure-Lambert fast path
        // returns baseColor without ever dividing), but nothing exact rests
        // on it -- the path's throughput still uses `weight` itself, so
        // checks 14/17's bit-exact 0.0625 are untouched, and this quantity
        // only feeds a Monte Carlo estimator.
        //
        // TWO DENSITIES ARE READ AT THIS ONE DIRECTION, on purpose, and
        // they are not interchangeable:
        //
        //   pdfEnvAtBsdfDir      = pdfEnvMap(...)      -> the MIS WEIGHT
        //   pdfEnvTexelAtBsdfDir = pdfEnvMapTexel(...) -> the RADIANCE
        //
        // pdfEnvMap divides the texel's CDF mass by sin(theta) of the QUERY
        // direction while that mass already carries sin(theta) of the texel
        // CENTRE (env_sampling.glsl's header; the behaviour is also written
        // up in site/content/units/sampling/env-cdf.md). For the weight
        // that is harmless -- both halves of a balance-heuristic partition
        // are formed from the same pair, so the sin ratio cancels out of
        // the weight, which is why the weight still uses it. For the
        // RADIANCE it is not: diffEnvRadianceFromPdf inverts
        // p = L*W*H/(integral*2*pi^2), an inversion valid ONLY for the
        // texel density, so feeding it pdfEnvMap's answer recovers
        // L * sin(theta_centre)/sin(theta_query) rather than L -- an error
        // reaching several times L near the poles, which Task 5 would
        // accumulate into the film as a near-pole energy bias and a firefly
        // source. pdfEnvMapTexel is the density with the query's sin
        // divided back out, and diff_gpu_probe.cpp check 31 asserts the
        // recovered value against the environment image texel by texel.
        if (pc.envWidth > 0u && pc.envHeight > 0u) {
            pdfEnvAtBsdfDir = pdfEnvMap(bsdfDir, pc.envWidth, pc.envHeight);
            pdfEnvTexelAtBsdfDir = pdfEnvMapTexel(bsdfDir, pc.envWidth, pc.envHeight);
        }
        if (pdf > 0.0) {
            visBsdf = diffShadowVisibility(shadowOrigin, bsdfDir);
        }
        bsdfRadiance = diffEnvRadianceFromPdf(pdfEnvTexelAtBsdfDir, pc.envWidth, pc.envHeight,
                                              pc.envIntegral);
        bsdfTerm = diffMisTerm(weight * pdf, vec3(bsdfRadiance), visBsdf, pdf, pdfEnvAtBsdfDir);

        // 1. Throughput decay by the BSDF estimator weight f*cos/pdf.
        // `pathThroughput` was read above, before the trace record, and is
        // the same field this used to re-read here -- nothing between the two
        // points writes it.
        arrivalThroughput = pathThroughput;
        psSetThroughput(pathIndex, pc.capacity, pathThroughput * weight);

        // 2. Advance to the hit point, offset along the geometric normal so
        //    the next trace cannot re-hit the face it just left.
        psSetOrigin(pathIndex, pc.capacity, hitPoint + normal * kSurfaceOffset);
        psSetDir(pathIndex, pc.capacity, newDir);

        // --- Fill the vertex the hook is about to see. Every value is the
        // one the traversal ACTUALLY used, taken from the variable it used,
        // not recomputed: a hook that re-derived any of these would be a
        // second implementation of the thing this file exists to have exactly
        // one of. `f` is f*cos reconstructed as weight*pdf, the same
        // reconstruction the BSDF-sampling MIS strategy above uses and for
        // the same reason (diffBsdfSample already returns f*cos/pdf).
        vtx.position = hitPoint;
        vtx.normal = normal;
        vtx.wi = newDir;
        vtx.bsdfDir = bsdfDir;
        vtx.f = weight * pdf;
        vtx.pdf = pdf;
        vtx.bsdfWeight = weight;
        vtx.roughness = roughness;
        vtx.metallic = metallic;
        // The two strategies' own inputs, each taken from the variable the
        // estimator above was actually formed from. A consumer summing the
        // corrected scatter line (see DiffVertex) needs every one of these:
        // without them the next-event half of that sum cannot be written,
        // because nothing else in the struct carries the light sample's
        // direction, density, radiance, visibility or MIS weight.
        vtx.envDir = envDir;
        vtx.envPdf = envPdf;
        vtx.envRadiance = envRadiance;
        vtx.visEnv = visEnv;
        vtx.wEnv = neeTerm.wOwn;
        vtx.bsdfRadiance = bsdfRadiance;
        vtx.visBsdf = visBsdf;
        vtx.wBsdf = bsdfTerm.wOwn;
    }

    // --- The single NEE-sink write. Every slot is named here and in
    // gpu_probe_context.hpp's NeeSampleSlot enum, and the two are TIED, not
    // merely asked not to drift: diff_gpu_probe.cpp's
    // checkWfScatterSinkLayoutTie parses these statements out of this source
    // and refuses to run the probe unless each offset carries the expression
    // its enumerator documents. That is what catches a transposition of two
    // same-arity slots, which leaves kNeeSampleFloats at 25 and would
    // otherwise pass every check below silently.
    const uint nb = pathIndex * kNeeSampleFloats;
    neeSamples.v[nb +  0u] = neeTerm.unweighted.x;
    neeSamples.v[nb +  1u] = neeTerm.unweighted.y;
    neeSamples.v[nb +  2u] = neeTerm.unweighted.z;
    neeSamples.v[nb +  3u] = neeTerm.wOwn;    // w_env at the light sample
    neeSamples.v[nb +  4u] = neeTerm.wOther;  // w_bsdf at the SAME direction
    neeSamples.v[nb +  5u] = bsdfTerm.unweighted.x;
    neeSamples.v[nb +  6u] = bsdfTerm.unweighted.y;
    neeSamples.v[nb +  7u] = bsdfTerm.unweighted.z;
    neeSamples.v[nb +  8u] = bsdfTerm.wOwn;    // w_bsdf at the BSDF sample
    neeSamples.v[nb +  9u] = bsdfTerm.wOther;  // w_env at the SAME direction
    neeSamples.v[nb + 10u] = envRadiance;
    neeSamples.v[nb + 11u] = pdfEnvAtBsdfDir;
    neeSamples.v[nb + 12u] = pdfBsdfAtEnvDir;
    neeSamples.v[nb + 13u] = visEnv;
    neeSamples.v[nb + 14u] = visBsdf;
    neeSamples.v[nb + 15u] = surfaceBranch;
    // The BSDF sample's own direction and density. Recorded because
    // pdfEnvMap's answer at slot 11 cannot be checked without the direction
    // it was asked about: env_sampling.glsl's pdfEnvMap divides the texel's
    // CDF mass by sin(theta) of the QUERY direction, while that mass
    // already carries sin(theta) of the texel CENTRE, so what it returns is
    // the texel density scaled by sin(theta_centre)/sin(theta_query) -- a
    // factor of 1 exactly at a texel centre and up to several elsewhere.
    // Nothing can verify that without knowing theta_query.
    neeSamples.v[nb + 16u] = bsdfSampleDir.x;
    neeSamples.v[nb + 17u] = bsdfSampleDir.y;
    neeSamples.v[nb + 18u] = bsdfSampleDir.z;
    neeSamples.v[nb + 19u] = bsdfSamplePdf;
    // The radiance the BSDF strategy actually multiplied in. Recorded
    // because slot 11's density alone cannot distinguish the two candidate
    // recoveries: L and L*sin(theta_centre)/sin(theta_query) differ by a
    // factor that is 1 at a texel centre and up to ~5 in the tail of a
    // cosine-weighted draw about +Y, so a check that only saw the density
    // would be blind to which one reached the estimator. Check 31 compares
    // this against the environment image at the texel bsdfSampleDir lands
    // in -- an exact per-sample identity, not a statistical one.
    neeSamples.v[nb + 20u] = bsdfRadiance;
    // The path's throughput on ARRIVAL at this vertex and the pixel this
    // path belongs to (Stage 0b-2b Task 5). Together with slots 0-9 these
    // are every factor of the film contribution below, recorded as
    // separate primitives rather than as the product, so that
    // diff_gpu_probe.cpp's film check reconstructs the contribution from
    // parts instead of comparing the accumulator against a copy of itself.
    // pixelIndex is a uint stored as a float: the film is indexed by it,
    // and capacities here are far below 2^24, so the round trip is exact.
    neeSamples.v[nb + 21u] = arrivalThroughput.x;
    neeSamples.v[nb + 22u] = arrivalThroughput.y;
    neeSamples.v[nb + 23u] = arrivalThroughput.z;
    neeSamples.v[nb + 24u] = float(pixelIndex);

    // --- THE PER-VERTEX HOOK ---------------------------------------
    //
    // This is where the forward pass's film accumulation used to be written
    // inline, and it is still exactly what the forward instantiation's hook
    // does -- see shaders/diff/wf_scatter.comp, which carries the whole of
    // the film note that used to sit here (which barrier orders the film
    // between bounces, why the caller owes it a zero-fill, and which of the
    // spec's three film-hazard options this subsystem took). It moved there
    // because accumulating radiance is what the FORWARD pass does at a
    // vertex; the replay pass does something else there, and the traversal
    // that gets both of them to the same vertex must not know which.
    //
    // `Lr` is formed HERE rather than in the hook so that both instantiations
    // are handed the identical quantity: it is the MIS-combined REFLECTED
    // direct radiance at this vertex, and the forward hook's
    // `throughput * Lr` is character-for-character the product the inline
    // film write computed.
    //
    // NOTE WHAT IT CONTAINS, because the name it carried for one task said
    // otherwise. `neeTerm.unweighted` and `bsdfTerm.unweighted` are each
    // `f*cos * L * V / p` (nee.glsl's diffMisTerm), so this sum has f in it
    // and has already been divided by both densities: it is L_r, not the
    // incident radiance L_i of spec 6.1's scatter line. The per-strategy
    // fields filled above are what that line is actually summed over.
    vtx.Lr = neeTerm.wOwn * neeTerm.unweighted + bsdfTerm.wOwn * bsdfTerm.unweighted;
    vtx.throughput = arrivalThroughput;
    diffVertexHook(vtx);

    // 3. Bounce count, incremented -- this is what the next dispatch's
    // fast-forward reads to know how many draws to skip. Incremented on the
    // miss path too, so the stream stays aligned with the draws taken above.
    psSetBounce(pathIndex, pc.capacity, bounce + 1u);

    // 4. Re-queue into the next bounce's ring. Every invocation reaching
    // this point always re-queues (see file header) -- nothing is filtered.
    // The counter itself is always incremented (so an overflow is visible/
    // diagnosable via readbackCounter), but the write is skipped once the
    // offset would land outside the destination ring's capacity-sized
    // allocation.
    const uint dstSlot = atomicAdd(counters.value[pc.dstCountSlot], 1u);
    if (dstSlot < pc.capacity) {
        queues.idx[pc.dstQueueBase + dstSlot] = pathIndex;
    }
}

#endif  // OHAO_DIFF_TRAVERSE_GLSL
