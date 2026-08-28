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
// density into radiance.
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

// PDF-only lookup for a given direction (needed for BSDF-side MIS).
float pdfEnvMap(vec3 dir, uint W, uint H) {
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
    float pdfUV = condDiff * margDiff * float(W) * float(H);
    float sinT = max(sin(theta), 1e-4);
    return max(pdfUV / (OHAO_TWOPI * OHAO_PI * sinT), 0.0);
}

#endif
