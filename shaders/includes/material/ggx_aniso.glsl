// Sub-plan 4.K: anisotropic-aware GGX D term.
//
// Used by the path-tracer raygen for direct-light NEE, env-MIS, and BSDF
// importance-sampling PDF computation. When anisotropy <= 0, returns the
// isotropic GGX formula (numerically equivalent to the inline form so
// pre-existing rendered references stay bit-for-bit identical at aniso=0).
//
// Tangent frame is reconstructed from the shading normal via Frisvad's basis
// (no geometric tangent attribute needed). This gives a consistent — though
// arbitrary — anisotropy direction; rotation can re-orient it per-shot via
// pc.jitter.w.

#ifndef GGX_ANISO_GLSL
#define GGX_ANISO_GLSL

// Coherent tangent field anchored to world-up. Frisvad's basis is fine for any
// math that doesn't care about direction (e.g. importance sampling) but it
// varies discontinuously with N — neighbouring pixels on a sphere see totally
// different T,B, so the anisotropy direction scrambles and the highlight
// averages out into something that looks isotropic. We project world-up onto
// the surface plane to get a tangent that smoothly varies across the surface
// (looks like a lathe-turned/rotational brush on spheres, brushed-along-grain
// on planes). Falls back to world-right at the poles where up ~ N.
void worldUpTangent(vec3 n, out vec3 t, out vec3 b) {
    vec3 up  = vec3(0.0, 1.0, 0.0);
    vec3 ref = abs(dot(up, n)) > 0.97 ? vec3(1.0, 0.0, 0.0) : up;
    t = normalize(ref - n * dot(ref, n));
    b = cross(n, t);
}

float ggxD_anisoOrIso(vec3 N, vec3 H, float NdotH, float roughness,
                      float anisotropy, float rotation) {
    if (anisotropy < 0.001) {
        float a     = roughness * roughness;
        float a2    = a * a;
        float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
        return a2 / (3.14159265 * denom * denom + 0.0001);
    }
    vec3 T, B;
    worldUpTangent(N, T, B);
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 Tr = T * c + B * s;
    vec3 Br = B * c - T * s;

    float r2     = roughness * roughness;
    float aspect = sqrt(1.0 - anisotropy * 0.9);
    float rT     = max(r2 / aspect, 0.001);
    float rB     = max(r2 * aspect, 0.001);

    float TdotH = dot(Tr, H);
    float BdotH = dot(Br, H);
    float d     = (TdotH * TdotH / rT) + (BdotH * BdotH / rB) + NdotH * NdotH;
    return 1.0 / (3.14159265 * rT * rB * d * d + 0.0001);
}

// ---------------------------------------------------------------------------
// GGX VNDF importance sampling (Heitz 2018, "Sampling the GGX Distribution of
// Visible Normals", JCGT). Used by the realtime raygen to draw the specular
// half-vector from the visible-normal distribution instead of a crude cosine
// lobe — a low-variance, importance-weighted single glossy sample. The alpha
// convention here (alpha = roughness^2) MATCHES ggxD_anisoOrIso's isotropic
// branch (a = roughness*roughness), so the sampled D and the pdf below agree.
// ---------------------------------------------------------------------------

// Isotropic GGX normal distribution term with alpha = roughness^2.
//
// The divide-by-zero guard is a FLOOR ON alpha, not an epsilon added to the
// denominator. It used to be `+ 1e-8` on the denominator, which is not a
// guard so much as a bias: for alpha <= ~0.024 (roughness <= ~0.155) the
// true denominator at the lobe's peak, pi*alpha^4, is itself of order 1e-6,
// so the epsilon suppressed D by around a percent exactly where the lobe is
// sharpest. That showed up as a measured 0.9% disagreement between this
// function and an independent double-precision GGX written from Walter et
// al. 2007 Eq. 33 (diff_gpu_probe.cpp's check 20, conductor at roughness
// 0.15), which is a real inaccuracy in the D term rather than float noise.
//
// The floor is safe without an epsilon because denom = (N.H)^2(a^2-1)+1 is
// bounded below by a^2 for every a <= 1 and by 1 for a >= 1, so with
// a^2 >= 1e-8 the denominator is at least pi*1e-16 ~ 3e-16 -- comfortably
// normal in float32. a^2 = 1e-8 is alpha = 1e-4, i.e. roughness = 0.01,
// which is exactly the floor pbr_unpack.glsl's unpackHitPbr already applies,
// so this changes nothing for any material that came through that unpack and
// only makes near-mirror lobes fractionally brighter and more accurate for
// any that did not.
//
// NOTE: ggxD_anisoOrIso's isotropic branch above still carries its own
// `+ 0.0001` and is therefore NO LONGER numerically identical to this
// function at sharp roughness. That epsilon is deliberately left alone: it
// is load-bearing for the pre-existing rendered references the RT pipeline
// is compared against, and nothing in the wavefront integrator calls it.
//
// THE TWO ARE NOT INTERCHANGEABLE, AND ONE MIS PARTITION SPANS BOTH. The
// `+ 0.0001` is far larger than the quantity it guards: at roughness 0.15
// the true denominator at the lobe peak is 8.05e-7, so the epsilon
// suppresses D by ~125x there. Integrated over the hemisphere that form
// retains 37% of the distribution's mass at roughness 0.2 and 2.7% at 0.1,
// against 100% for the alpha-floored form below (quadrature figures from
// site/content/units/materials/ggx.md). Any place that computes the SAME
// physical quantity on both sides of a MIS weight must therefore use the
// SAME function on both sides, or the balance heuristic is evaluated with
// two different densities and the estimator is silently biased.
// pt_raygen_realtime.rgen is exactly such a case: its env-IS branch's
// `Dpdf`/`pdfSpecMIS` and its VNDF sampler's `specLastBsdfPdf` are the two
// sides of one balance heuristic, both are the specular BSDF sampler's VNDF
// pdf, and both now call this function for it. `spec` (the shaded BSDF
// value, not a density) still uses ggxD_anisoOrIso, because that is the
// value the reference renders were made with.
//
// COVERAGE, stated plainly so a green test suite is not misread. In the RT
// pipeline this function is reached only from pt_raygen_realtime.rgen (the
// VNDF sampler's pdf, and -- as of this fix -- the env-IS branch's
// `pdfSpecMIS`), and NOTHING in the C++ test suite executes it.
// tests/renderer/renderer_test.cpp selects RenderMode::RTOffline, i.e.
// pt_raygen_offline.rgen, which contains no ggxDiso call at all; that test
// also asserts nothing about pixels (it writes a PNG and returns 0
// unconditionally) and falls back silently when RT is unavailable. The golden-image corpus (tests/golden/manifest.json) also
// renders RTOffline only. The only automated coverage of this function is
// the wavefront differentiable path -- shaders/includes/diff/bsdf.glsl via
// tests/diff/diff_gpu_probe.cpp's checks 20-23 -- which shares the formula
// but not the raygen. A change here at roughness in [0.02, ~0.155] is
// therefore exercised by nothing on the RT side: verify it by eye or with a
// new test, never by a green renderer_test.
float ggxDiso(float NdotH, float alpha) {
    float a2    = max(alpha * alpha, 1e-8);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * denom * denom);
}

// Smith masking-shadowing auxiliary (GGX Lambda). cosTheta = N·w.
float smithLambdaGGX(float cosTheta, float alpha) {
    float c2   = cosTheta * cosTheta;
    float tan2 = max(0.0, 1.0 - c2) / max(c2, 1e-8);
    return 0.5 * (-1.0 + sqrt(1.0 + alpha * alpha * tan2));
}

// Monodirectional Smith masking G1(w).
float smithG1GGX(float cosTheta, float alpha) {
    return 1.0 / (1.0 + smithLambdaGGX(cosTheta, alpha));
}

// Height-correlated Smith G2(V,L) divided by G1(V). This is exactly the
// estimator weight f*cos/pdf for a VNDF-sampled microfacet reflection once the
// Fresnel term is factored out (see the raygen). Both <= 1 and equal at the
// smooth limit (→ 1).
float smithG2overG1GGX(float NdotV, float NdotL, float alpha) {
    float lambdaV = smithLambdaGGX(NdotV, alpha);
    float lambdaL = smithLambdaGGX(NdotL, alpha);
    return (1.0 + lambdaV) / (1.0 + lambdaV + lambdaL + 1e-8);
}

// Arbitrary orthonormal basis around N (isotropic sampling doesn't care about
// the tangent direction). Matches cosineHemisphere's convention.
void ggxBuildBasis(vec3 N, out vec3 T, out vec3 B) {
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// Sample the GGX distribution of visible normals. `Ve` is the view direction in
// the local frame where N = (0,0,1) and must have Ve.z > 0. Returns the sampled
// half-vector H in the same local frame. (Heitz 2018, listing in Section 4.)
vec3 sampleGGXVNDF(vec3 Ve, float ax, float ay, vec2 u) {
    // Section 3.2: stretch the view direction to the hemisphere configuration.
    vec3 Vh = normalize(vec3(ax * Ve.x, ay * Ve.y, Ve.z));
    // Section 4.1: orthonormal basis (degenerate-safe).
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq)
                          : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(Vh, T1);
    // Section 4.2: parameterize the projected area (disk with vertical squash).
    float r   = sqrt(u.x);
    float phi = 2.0 * 3.14159265 * u.y;
    float t1  = r * cos(phi);
    float t2  = r * sin(phi);
    float s   = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
    // Section 4.3: reproject onto the hemisphere.
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    // Section 3.4: unstretch back to the ellipsoid configuration.
    return normalize(vec3(ax * Nh.x, ay * Nh.y, max(0.0, Nh.z)));
}

#endif // GGX_ANISO_GLSL
