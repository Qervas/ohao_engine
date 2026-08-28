#ifndef OHAO_ENV_SAMPLING_GLSL
#define OHAO_ENV_SAMPLING_GLSL

// This header declares NO bindings and reads NO push constants of its own.
// The only caller-provided symbols it references are `envMarg.data` and
// `envCond.data`, which the caller must declare BEFORE including it:
//
//   layout(std430, ...) readonly buffer EnvMarginalCDF    { float data[]; } envMarg;
//   layout(std430, ...) readonly buffer EnvConditionalCDF { float data[]; } envCond;
//
// The binding indices are the caller's to choose. The RT pipeline's raygen
// and miss shaders use set=0 bindings 17 and 18; shaders/diff/wf_scatter.comp
// uses 4 and 5 in its own set. Nothing here depends on which.
//
// The map's dimensions and integral are ORDINARY ARGUMENTS of the entry
// points below, not push-constant reads, so a caller supplies them from
// wherever it keeps them. The RT shaders pass (pc.control.w,
// uint(pc.tuning.y), pc.tuning.z); wf_scatter.comp passes its own
// (pc.envWidth, pc.envHeight, pc.envIntegral). Note that `envIntegral` is
// currently accepted by sampleEnvMap and then unused -- the solid-angle pdf
// is recovered from CDF differences, which are already normalised, and
// pdfEnvMap does not take it at all. It is kept in the signature because
// callers pass it and a next-event estimator needs it to convert the CDF's
// density into radiance. That consumer now exists:
// shaders/includes/diff/nee.glsl inverts the relation below to recover grey
// radiance as pdf * integral * 2*pi^2 / (W*H), and diff_gpu_probe.cpp check
// 31 asserts the result against the environment image -- which is the first
// thing anywhere to verify that a caller-supplied integral reaches the GPU
// intact.
//
// pdfEnvMap IS NOT EXACTLY sampleEnvMap S DENSITY OFF A TEXEL CENTRE. Its
// condDiff*margDiff already carries sin(theta) of the texel CENTRE (that is
// how the CDF is weighted), and it then divides by sin(theta) of the QUERY
// direction, so what it returns is the texel density scaled by
// sin(theta_centre)/sin(theta_query). The factor is exactly 1 at a texel
// centre -- which is the only place sampleEnvMap ever puts a sample, so the
// two agree on the environment strategy s entire support and MIS weights
// built from them still partition unity -- and can reach several elsewhere,
// nearest the poles.
//
// THIS IS NOT A NEW FINDING. site/content/units/sampling/env-cdf.md
// (published as site/m/sampling/env-cdf.html) already states it, works the
// pole case to a factor of 7.7 between the two sides of one weight for a
// 2048-high map, and records that sampleEnvMap emits texel centres so the
// environment strategy s expectation is a midpoint quadrature rather than
// the integral. diff_gpu_probe.cpp check 31 is the first thing to ASSERT
// the relation on a GPU run -- pdfEnvMap had no caller under test anywhere
// in this repository before it -- but the behaviour was documented first.
//
// WHICH OF THE TWO A CALLER WANTS depends entirely on what it does with the
// answer, and getting that wrong is a bias, not a nicety:
//
//   * A BALANCE-HEURISTIC WEIGHT wants pdfEnvMap. The weight only has to
//     partition unity pointwise, and both halves of one partition are
//     formed from the same density pair, so the sin ratio cancels out of
//     the weight entirely.
//   * RECOVERING RADIANCE by inverting the density (shaders/includes/diff/
//     nee.glsl s diffEnvRadianceFromPdf) wants pdfEnvMapTexel. That
//     inversion is only valid for the TEXEL density: feeding it pdfEnvMap s
//     answer at an off-centre direction recovers
//     L * sin(theta_centre)/sin(theta_query), not L -- an energy error that
//     reaches several times L near the poles and a firefly source once the
//     result is accumulated into a film. shaders/diff/wf_scatter.comp did
//     exactly that for one commit; pdfEnvMapTexel exists so no caller has
//     to reconstruct sin(theta_centre) from the binning by hand.
//
// CDF CONVENTION, which the host-side builder must match exactly
// (ohao/render/rt/env_cdf.cpp is the one this repository uses):
//   * `envCond` is H rows of W floats, each row an INCLUSIVE cumulative
//     distribution NORMALISED WITHIN THAT ROW, so envCond[y*W + W-1] == 1.
//   * `envMarg` is H floats, an INCLUSIVE cumulative distribution over rows,
//     normalised over the whole map, so envMarg[H-1] == 1.
//   * Both are built from luminance weighted by sin(theta), so the
//     solid-angle density below comes out proportional to luminance.

const float OHAO_PI    = 3.14159265358979;
const float OHAO_TWOPI = 6.28318530717959;

// Binary search over the marginal CDF (rows). Returns row index in [0, H).
int searchMarginal(uint H, float u) {
    int lo = 0;
    int hi = int(H) - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (envMarg.data[uint(mid)] < u) lo = mid + 1;
        else                             hi = mid;
    }
    return lo;
}

// Binary search over a row's conditional CDF (columns). Returns column index in [0, W).
int searchConditional(uint rowBase, uint W, float u) {
    int lo = 0;
    int hi = int(W) - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (envCond.data[rowBase + uint(mid)] < u) lo = mid + 1;
        else                                        hi = mid;
    }
    return lo;
}

// Convert (x, y) pixel coord to world-space direction on the unit sphere.
vec3 equirectPixelToDir(int x, int y, uint W, uint H) {
    float u = (float(x) + 0.5) / float(W);
    float v = (float(y) + 0.5) / float(H);
    float phi   = (u - 0.5) * OHAO_TWOPI;    // [-pi, pi]
    float theta = v * OHAO_PI;               // [0, pi], theta=0 at +Y, pi at -Y
    float sinT = sin(theta);
    return vec3(sinT * cos(phi),
                cos(theta),
                sinT * sin(phi));
}

// Sample the env map proportional to luminance. Returns direction + PDF (solid angle).
void sampleEnvMap(float u1, float u2, uint W, uint H, float envIntegral,
                  out vec3 dir, out float pdf) {
    int y = searchMarginal(H, u1);
    uint rowBase = uint(y) * W;
    int x = searchConditional(rowBase, W, u2);

    dir = equirectPixelToDir(x, y, W, H);

    // PDF in solid-angle measure
    float theta = (float(y) + 0.5) / float(H) * OHAO_PI;
    float sinT = max(sin(theta), 1e-4);

    // Recover luminance-proportional density at this texel from CDF steps.
    float condDiff = envCond.data[rowBase + uint(x)]
                   - (x > 0 ? envCond.data[rowBase + uint(x - 1)] : 0.0);
    float margDiff = envMarg.data[uint(y)]
                   - (y > 0 ? envMarg.data[uint(y - 1)] : 0.0);
    float pdfUV = condDiff * margDiff * float(W) * float(H);  // density in UV space

    // Jacobian from UV to solid angle: 2*pi*pi*sin(theta)
    pdf = pdfUV / (OHAO_TWOPI * OHAO_PI * sinT);
    pdf = max(pdf, 0.0);
}

// Bin `dir` back to a texel and return that texel's CDF mass as a UV-space
// density, together with BOTH polar angles a caller might divide it by: the
// angle of the direction that was ASKED about, and the angle of the CENTRE
// of the texel it landed in. The two functions below differ ONLY in which
// one they use -- written once here rather than twice below, so that a
// change to the binning cannot reach one of them and not the other.
float envTexelPdfUV(vec3 dir, uint W, uint H, out float thetaQuery, out float thetaCentre) {
    // Reverse of equirectPixelToDir
    float theta = acos(clamp(dir.y, -1.0, 1.0));         // [0, pi]
    float phi   = atan(dir.z, dir.x);                    // [-pi, pi]
    float u = phi / OHAO_TWOPI + 0.5;
    float v = theta / OHAO_PI;
    int x = clamp(int(u * float(W)), 0, int(W) - 1);
    int y = clamp(int(v * float(H)), 0, int(H) - 1);
    uint rowBase = uint(y) * W;

    float condDiff = envCond.data[rowBase + uint(x)]
                   - (x > 0 ? envCond.data[rowBase + uint(x - 1)] : 0.0);
    float margDiff = envMarg.data[uint(y)]
                   - (y > 0 ? envMarg.data[uint(y - 1)] : 0.0);
    thetaQuery  = theta;
    // The same expression equirectPixelToDir uses for the row it is handed,
    // so "the centre of the texel `dir` landed in" means the same thing to
    // this function and to the sampler.
    thetaCentre = (float(y) + 0.5) / float(H) * OHAO_PI;
    return condDiff * margDiff * float(W) * float(H);
}

// PDF-only lookup for a given direction (needed for BSDF-side MIS).
//
// Divides by sin(theta) of the QUERY direction. Unchanged, bit for bit,
// from before envTexelPdfUV was factored out of it: same operations, same
// order. Use this for MIS weights; use pdfEnvMapTexel to recover radiance.
float pdfEnvMap(vec3 dir, uint W, uint H) {
    float thetaQuery;
    float thetaCentre;
    float pdfUV = envTexelPdfUV(dir, W, H, thetaQuery, thetaCentre);
    float sinT = max(sin(thetaQuery), 1e-4);
    return max(pdfUV / (OHAO_TWOPI * OHAO_PI * sinT), 0.0);
}

// The density sampleEnvMap ITSELF would report for the texel `dir` lands in:
// the piecewise-constant solid-angle density the CDF was built from,
//
//     p(x,y) = L_grey(x,y) * W * H / (integral * 2*pi^2),
//
// with no dependence on where inside the texel `dir` fell. Identical to
// pdfEnvMap at a texel centre and to sampleEnvMap's returned pdf for the
// texel it chose; elsewhere it is pdfEnvMap divided by
// sin(theta_centre)/sin(theta_query).
//
// This is the one to invert when recovering radiance from a density, and
// the reason the two are separate functions rather than one with a flag:
// the choice is not a tuning knob, it follows from whether the caller is
// forming a ratio (where the sin factor cancels) or a magnitude (where it
// does not).
float pdfEnvMapTexel(vec3 dir, uint W, uint H) {
    float thetaQuery;
    float thetaCentre;
    float pdfUV = envTexelPdfUV(dir, W, H, thetaQuery, thetaCentre);
    float sinT = max(sin(thetaCentre), 1e-4);
    return max(pdfUV / (OHAO_TWOPI * OHAO_PI * sinT), 0.0);
}

#endif
