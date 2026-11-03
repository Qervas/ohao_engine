// Von Mises-Fisher lobes and the guiding state that learns one (sub-project 23, path guiding;
// docs/specs/2026-11-02-path-guiding-design.md). Sources: Alber, Hanika, Dachsbacher, "Real-Time
// Markov Chain Path Guiding for Global Illumination and Single Scattering" (2025), section 4 and
// Algorithms 2-3; Ruppert, Herholz, Lensch (2020) for the sufficient statistics; Banerjee et al.
// (2005, eq. 4.4) for the concentration; Jakob (2012), "Numerically stable sampling of the von
// Mises-Fisher distribution on S^2". tools/sampling/vmf_kernel.slang runs it on the CPU for
// tests/renderer/vmf_test.

static const float kVmfPi = 3.14159265358979;
static const float kVmfKappaMin = 1e-4;       // below it the lobe is uniform over the sphere
static const float kVmfKappaMax = 1e4;        // a cap on the fit's concentration
static const float kVmfRbarMax = 0.9999;      // the fit's mean cosine is clamped below 1

// The density over the sphere: kappa / (4 pi sinh kappa) exp(kappa dot(dir, mu)), written so that
// neither exponential overflows: kappa / (2 pi (1 - exp(-2 kappa))) exp(kappa (dot - 1)).
float vmfPdf(float3 dir, float3 mu, float kappa)
{
    if (kappa < kVmfKappaMin) return 1.0 / (4.0 * kVmfPi);
    const float e = exp(-2.0 * kappa);
    return kappa / (2.0 * kVmfPi * (1.0 - e)) * exp(kappa * (dot(dir, mu) - 1.0));
}

// An orthonormal basis around n (Duff et al. 2017).
void vmfBasis(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// A direction drawn with density vmfPdf(., mu, kappa) from two uniforms in [0, 1).
float3 vmfSample(float3 mu, float kappa, float2 u)
{
    // t1 = 1 - w, the cosine's distance from 1, kept apart so a sharp lobe's sine is not lost to
    // rounding w near 1 (Jakob 2012, eq. 9).
    float t1;
    if (kappa < kVmfKappaMin) t1 = 2.0 * u.x;
    else t1 = -log(u.x + (1.0 - u.x) * exp(-2.0 * kappa)) / kappa;
    t1 = clamp(t1, 0.0, 2.0);
    const float w = 1.0 - t1;
    const float s = sqrt(max(t1 * (2.0 - t1), 0.0));
    const float phi = 2.0 * kVmfPi * u.y;
    float3 bt, bb;
    vmfBasis(mu, bt, bb);
    return normalize(w * mu + s * (cos(phi) * bt + sin(phi) * bb));
}

// The mean cosine of a lobe of concentration kappa: coth(kappa) - 1/kappa.
float vmfMeanCosine(float kappa)
{
    if (kappa < 1e-3) return kappa / 3.0;
    const float e = exp(-2.0 * kappa);
    return (1.0 + e) / (1.0 - e) - 1.0 / kappa;
}

// The concentration whose lobe has mean cosine rbar, capped: Banerjee et al.'s approximation
// (2005, eq. 4.4; up to 4.9% off near rbar 0.8) and one Newton step on vmfMeanCosine (within 0.25%).
float vmfKappa(float rbar)
{
    const float r = clamp(rbar, 0.0, kVmfRbarMax);
    float k = (3.0 * r - r * r * r) / (1.0 - r * r);
    if (k > 1e-3)
    {
        const float e = exp(-2.0 * k);
        const float dA = 1.0 / (k * k) - 4.0 * e / ((1.0 - e) * (1.0 - e));   // d/dk (coth k - 1/k)
        if (dA > 0.0) k = max(k - (vmfMeanCosine(k) - r) / dA, 0.5 * k);
    }
    return min(k, kVmfKappaMax);
}

// ---- the guiding state (the paper's Table 1) -------------------------------------------------

static const uint kGuideNMax = 64u;          // samples a state counts before its blend stops shrinking
static const float kGuideAlphaMin = 0.02;    // the smallest blend: a state keeps following change

// One Markov chain state: sufficient statistics of one vMF lobe, meaningful at any nearby point.
// 32 bytes, the layout of the grid's buffers.
struct GuideState
{
    float w;        // luminance estimate of the incident light (the resampling weight); 0: empty
    float3 y;       // w-weighted mean of the points the light came from (world)
    float r;        // w-weighted mean cosine to the lobe's mean
    uint nt;        // samples (low 16 bits, to kGuideNMax) and the frame of the last update (high 16)
    uint check;     // the cell's second hash: a slot another cell took reads as empty
    uint pad;
};

GuideState guideEmpty()
{
    GuideState s;
    s.w = 0.0;
    s.y = float3(0.0);
    s.r = 0.0;
    s.nt = 0u;
    s.check = 0u;
    s.pad = 0u;
    return s;
}

uint guideCount(GuideState s) { return s.nt & 0xFFFFu; }
uint guideFrame(GuideState s) { return s.nt >> 16u; }

// The state's lobe seen from x (Algorithm 2, lines 6-8): toward the mean point, its mean cosine
// pulled toward 0 by a prior of n0 + 1 / distance^2 samples (the paper's is 1 / distance^2 alone: a
// far, small light sharpens fast; n0 keeps a state of one sample from being a needle).
// False when the state is empty.
bool guideLobe(GuideState s, float3 x, float n0, out float3 mu, out float kappa)
{
    mu = float3(0.0, 0.0, 1.0);
    kappa = 0.0;
    if (!(s.w > 0.0)) return false;
    const float3 d = s.y / s.w - x;
    const float d2 = dot(d, d);
    if (!(d2 > 1e-12)) return false;
    mu = d * rsqrt(d2);
    const float n = float(guideCount(s));
    const float np = n0 + 1.0 / d2;
    const float rbar = (n * n * (s.r / s.w)) / (n * n + np);
    kappa = vmfKappa(rbar);
    return true;
}

// The maximum-likelihood step (Algorithm 3, lines 4-10): one sample of luminance f from x that
// reached `target` blended into the state. The cosine is taken against the lobe's mean before the
// step (the sample's own direction when the state was empty).
GuideState guideUpdate(GuideState s, float3 x, float3 target, float f, uint frame)
{
    const float3 dir = normalize(target - x);
    float3 mu = dir;
    if (s.w > 0.0)
    {
        const float3 d = s.y / s.w - x;
        if (dot(d, d) > 1e-12) mu = normalize(d);
    }
    const uint n = min(guideCount(s) + 1u, kGuideNMax);
    const float alpha = max(1.0 / float(n), kGuideAlphaMin);
    GuideState o = s;
    o.w = lerp(s.w, f, alpha);
    o.y = lerp(s.y, f * target, alpha);
    o.r = lerp(s.r, f * dot(dir, mu), alpha);
    o.nt = n | ((frame & 0xFFFFu) << 16u);
    return o;
}
