// OpenPBR Surface 1.1.1's base (docs/specs/2026-10-29-openpbr-materials-design.md, step 1): one
// definition of a surface for every renderer. The path tracer, deferred lighting, the forward
// transparent pass and RT GI's hits import it; tests/material/ runs it on the CPU (slangc's C++
// target). Every direction is in the local shading frame, z the shading normal; wo points to the
// viewer, wi to the light, both away from the surface.
//
// The base, as OpenPBR layers it:
//   metal       GGX with the F82-tint Fresnel (Kutz 2021): F0 = base_weight * base_color, the
//               reflectance at mu = 1/7 is specular_color times Schlick's, all times
//               specular_weight.
//   dielectric  GGX with the exact Fresnel of the IOR specular_ior modified by specular_weight
//               (its F0 scaled, clamped below 1), tinted by specular_color on the reflection from
//               above only; under it, by albedo scaling at wo, (1 - E_spec(wo)) * EON.
//   mixed by base_metalness.
// GGX is single scatter plus Kulla-Conty's multiple-scattering lobe, with Fresnel's average
// (Kulla & Conty 2017), so a white metal and a white dielectric over white EON reflect exactly
// what they receive (the white furnace). The compensation's tables (E1, its average, the F82
// moments, the dielectric's single-scatter albedo) are generated from this module by
// tools/material/openpbr_tables and read through OpenPBRTables.
//
// Albedo scaling at wo alone is OpenPBR's: the layered BSDF is not reciprocal; each lobe is.

static const float kPbrPi = 3.14159265358979;
static const float kPbrInvPi = 0.318309886183791;
// Below this roughness the GGX lobe is treated at it: alpha 1e-4, a mirror to any renderer's
// resolution, without a delta lobe's special cases.
static const float kMinSpecularRoughness = 0.01;

// ---- the energy tables ----------------------------------------------------------------------

// Layout of the tables buffer (floats), the generator's and the lookup's one contract. A mu axis
// is sampled at mu = (i / (n - 1))^2, looked up by sqrt(mu): Fresnel and the albedos change
// fastest at grazing (a linear 16-point axis left an IOR 3 mirror 1.1% off at 85 degrees).
static const uint kPbrTableMu = 32u;       // E1, S5, S6: [roughness][mu]
static const uint kPbrTableRough = 32u;
static const uint kPbrTableEta = 32u;      // FavgD: eta in [1, 3]
static const uint kPbrDielMu = 16u;        // EssD: [eta][roughness][mu]
static const uint kPbrDielRough = 16u;
static const uint kPbrDielEta = 16u;
static const float kPbrEtaMax = 3.0;
static const uint kPbrOffE1 = 0u;
static const uint kPbrOffS5 = kPbrOffE1 + kPbrTableMu * kPbrTableRough;
static const uint kPbrOffS6 = kPbrOffS5 + kPbrTableMu * kPbrTableRough;
static const uint kPbrOffE1Avg = kPbrOffS6 + kPbrTableMu * kPbrTableRough;
static const uint kPbrOffFavgD = kPbrOffE1Avg + kPbrTableRough;
static const uint kPbrOffEssD = kPbrOffFavgD + kPbrTableEta;
static const uint kPbrOffEssDLow = kPbrOffEssD + kPbrDielMu * kPbrDielRough * kPbrDielEta;
static const uint kPbrAnisoA = 8u;         // AnisoRough: [aniso][rough][mu][phi], aniso at 1 - (1 - g)^2
static const uint kPbrAnisoRough = 16u;
static const uint kPbrAnisoMu = 16u;
static const uint kPbrAnisoPhi = 8u;       // phi over [0, pi/2] from the tangent
static const uint kPbrOffAnisoRough = kPbrOffEssDLow + kPbrDielMu * kPbrDielRough * kPbrDielEta;
static const uint kPbrOffAnisoE1Avg = kPbrOffAnisoRough + kPbrAnisoA * kPbrAnisoRough * kPbrAnisoMu * kPbrAnisoPhi;
static const uint kPbrTableFloats = kPbrOffAnisoE1Avg + kPbrAnisoA * kPbrAnisoRough;
// EssDLow's eta axis: 1 + kPbrLowEtaSpan g^2 over the grid g, below which EssD is read from it (a
// base under a coat sees an IOR near 1, where grazing Fresnel changes fastest).
static const float kPbrLowEtaSpan = 0.3;

// A grid's coordinate for x in [0, 1] over n samples at i / (n - 1): the lower index and the
// fraction toward the next.
void pbrGridCoord(float x, uint n, out uint i, out float f)
{
    const float g = saturate(x) * float(n - 1u);
    i = min(uint(g), n - 2u);
    f = g - float(i);
}

struct OpenPBRTables
{
    StructuredBuffer<float> data;   // kPbrTableFloats

    float lerp1(uint off, uint n, float x)
    {
        uint i;
        float f;
        pbrGridCoord(x, n, i, f);
        return lerp(data[off + i], data[off + i + 1u], f);
    }

    // A [rough][mu] table, bilinear.
    float lerp2(uint off, float mu, float rough)
    {
        uint im, ir;
        float fm, fr;
        pbrGridCoord(sqrt(saturate(mu)), kPbrTableMu, im, fm);
        pbrGridCoord(rough, kPbrTableRough, ir, fr);
        const uint r0 = off + ir * kPbrTableMu, r1 = r0 + kPbrTableMu;
        return lerp(lerp(data[r0 + im], data[r0 + im + 1u], fm), lerp(data[r1 + im], data[r1 + im + 1u], fm), fr);
    }

    // GGX single scatter's directional albedo with F = 1.
    float E1(float mu, float rough) { return lerp2(kPbrOffE1, mu, rough); }
    // Its cosine-weighted average, 2 * integral of E1(mu) mu dmu over the interpolated E1.
    float E1Avg(float rough) { return lerp1(kPbrOffE1Avg, kPbrTableRough, rough); }
    // Single scatter's albedo weighted by (1 - m.wo)^5 and by (m.wo)(1 - m.wo)^6: with E1 the
    // F82 metal's single-scatter albedo is exact (it is linear in F0 and its b).
    float S5(float mu, float rough) { return lerp2(kPbrOffS5, mu, rough); }
    float S6(float mu, float rough) { return lerp2(kPbrOffS6, mu, rough); }
    // The exact dielectric Fresnel's cosine-weighted average at eta in [1, kPbrEtaMax].
    float FavgD(float eta) { return lerp1(kPbrOffFavgD, kPbrTableEta, (eta - 1.0) / (kPbrEtaMax - 1.0)); }
    // The dielectric's single-scatter albedo (exact Fresnel), trilinear in (mu, rough, eta); below
    // 1 + kPbrLowEtaSpan from EssDLow.
    float EssD(float mu, float rough, float eta)
    {
        uint im, ir, ie;
        float fm, fr, fe;
        pbrGridCoord(sqrt(saturate(mu)), kPbrDielMu, im, fm);
        pbrGridCoord(rough, kPbrDielRough, ir, fr);
        const bool low = eta < 1.0 + kPbrLowEtaSpan;
        if (low) pbrGridCoord(sqrt(max(eta - 1.0, 0.0) / kPbrLowEtaSpan), kPbrDielEta, ie, fe);
        else pbrGridCoord((eta - 1.0) / (kPbrEtaMax - 1.0), kPbrDielEta, ie, fe);
        const uint off = low ? kPbrOffEssDLow : kPbrOffEssD;
        float v[2];
        [unroll]
        for (uint k = 0u; k < 2u; ++k)
        {
            const uint e0 = off + (ie + k) * kPbrDielRough * kPbrDielMu;
            const uint r0 = e0 + ir * kPbrDielMu, r1 = r0 + kPbrDielMu;
            v[k] = lerp(lerp(data[r0 + im], data[r0 + im + 1u], fm), lerp(data[r1 + im], data[r1 + im + 1u], fm), fr);
        }
        return lerp(v[0], v[1], fe);
    }

    // The isotropic roughness whose E1 is an anisotropic GGX lobe's single-scatter albedo seen
    // from w (OpenPBR's alpha mapping of (rough, aniso)), quadrilinear: every table is read at it.
    float AnisoRough(float aniso, float rough, float3 w)
    {
        uint ia, ir, im, ip;
        float fa, fr, fm, fp;
        pbrGridCoord(1.0 - sqrt(saturate(1.0 - aniso)), kPbrAnisoA, ia, fa);
        pbrGridCoord(rough, kPbrAnisoRough, ir, fr);
        pbrGridCoord(sqrt(saturate(w.z)), kPbrAnisoMu, im, fm);
        pbrGridCoord(atan2(abs(w.y), abs(w.x)) * (2.0 / kPbrPi), kPbrAnisoPhi, ip, fp);
        float v = 0.0;
        [unroll]
        for (uint k = 0u; k < 16u; ++k)
        {
            const uint da = k & 1u, dr = (k >> 1u) & 1u, dm = (k >> 2u) & 1u, dp = k >> 3u;
            const float w4 = (da == 1u ? fa : 1.0 - fa) * (dr == 1u ? fr : 1.0 - fr) * (dm == 1u ? fm : 1.0 - fm)
                           * (dp == 1u ? fp : 1.0 - fp);
            const uint i = (((ia + da) * kPbrAnisoRough + ir + dr) * kPbrAnisoMu + im + dm) * kPbrAnisoPhi + ip + dp;
            v += w4 * data[kPbrOffAnisoRough + i];
        }
        return v;
    }

    // E1's cosine-weighted average over every direction, read at AnisoRough.
    float AnisoE1Avg(float aniso, float rough)
    {
        uint ia, ir;
        float fa, fr;
        pbrGridCoord(1.0 - sqrt(saturate(1.0 - aniso)), kPbrAnisoA, ia, fa);
        pbrGridCoord(rough, kPbrAnisoRough, ir, fr);
        const uint r0 = kPbrOffAnisoE1Avg + ia * kPbrAnisoRough + ir, r1 = r0 + kPbrAnisoRough;
        return lerp(lerp(data[r0], data[r0 + 1u], fr), lerp(data[r1], data[r1 + 1u], fr), fa);
    }
};

// The roughness the isotropic tables are read at for a GGX lobe seen from w, and E1's average for
// it: the lobe's own when isotropic.
float pbrTableRough(OpenPBRTables t, float rough, float anisotropy, float3 w)
{
    return anisotropy > 0.0 ? t.AnisoRough(anisotropy, rough, w) : rough;
}

float pbrE1Avg(OpenPBRTables t, float rough, float anisotropy)
{
    return anisotropy > 0.0 ? t.AnisoE1Avg(anisotropy, rough) : t.E1Avg(rough);
}

// ---- GGX ------------------------------------------------------------------------------------

// D for a unit microfacet normal m in the local frame, written in m's components:
// a2 / (pi (sin^2 + cos^2 a2)^2). The textbook NoH^2 (a2 - 1) + 1 rounds a2 - 1 to -1 at small
// alpha (alpha 1e-4: a2 1e-8), and the peak became 0 / 0.
float ggxD(float3 m, float a2)
{
    const float d = dot(m.xy, m.xy) + m.z * m.z * a2;
    return a2 / (kPbrPi * d * d);
}

// Smith's Lambda for GGX at a direction of cosine mu > 0.
float ggxLambda(float mu, float a2)
{
    const float mu2 = mu * mu;
    return 0.5 * (sqrt(1.0 + a2 * (1.0 - mu2) / mu2) - 1.0);
}

// Single scatter's BSDF x cos(wi) without Fresnel: D G2 / (4 mu_o), height-correlated G2.
float ggxSingle(float3 wo, float3 wi, float a2)
{
    const float3 m = normalize(wo + wi);
    const float G2 = 1.0 / (1.0 + ggxLambda(wo.z, a2) + ggxLambda(wi.z, a2));
    return ggxD(m, a2) * G2 / (4.0 * wo.z);
}

// Bounded VNDF sampling (Eto & Tokuyoshi 2023): a microfacet normal for the view wo, from a
// spherical cap that leaves out the normals whose reflection falls below the horizon.
float3 ggxSampleBounded(float3 wo, float alpha, float2 u)
{
    const float3 woStd = normalize(float3(wo.xy * alpha, wo.z));
    const float phi = (2.0 * u.x - 1.0) * kPbrPi;
    const float a = saturate(alpha);
    const float s = 1.0 + length(wo.xy);
    const float a2 = a * a, s2 = s * s;
    const float k = (1.0 - a2) * s2 / (s2 + a2 * wo.z * wo.z);
    const float b = wo.z > 0.0 ? k * woStd.z : woStd.z;
    const float z = (1.0 - u.y) * (1.0 + b) - b;
    const float sinTheta = sqrt(saturate(1.0 - z * z));
    const float3 mStd = float3(sinTheta * cos(phi), sinTheta * sin(phi), z) + woStd;
    return normalize(float3(mStd.xy * alpha, mStd.z));
}

// The probability density of wi = reflect(-wo, m) under ggxSampleBounded.
float ggxPdfBounded(float3 wo, float3 wi, float alpha)
{
    const float3 m = normalize(wo + wi);
    const float a2 = alpha * alpha;
    const float ndf = ggxD(m, a2);
    const float2 ao = alpha * wo.xy;
    const float len2 = dot(ao, ao);
    const float t = sqrt(len2 + wo.z * wo.z);
    if (wo.z >= 0.0)
    {
        const float a = saturate(alpha);
        const float s = 1.0 + length(wo.xy);
        const float sa2 = a * a, s2 = s * s;
        const float k = (1.0 - sa2) * s2 / (s2 + sa2 * wo.z * wo.z);
        return ndf / (2.0 * (k * wo.z + t));
    }
    return ndf * (t - wo.z) / (2.0 * len2);
}

// ---- anisotropic GGX ------------------------------------------------------------------------
// The same lobe stretched along the frame's x (the tangent) by a = (alpha_t, alpha_b). A material
// without anisotropy never reaches these (OpenPBRBase and OpenPBRCoat branch), so the isotropic
// arithmetic above stays bit for bit what it was.

// OpenPBR's mapping: alpha_t = r^2 sqrt(2 / (1 + (1 - a)^2)), alpha_b = (1 - a) alpha_t, which keeps
// alpha_t^2 + alpha_b^2 = 2 alpha^2. alpha_b is held at 1e-4 (a mirror), as the isotropic floor.
float2 ggxAnisoAlpha(float rough, float anisotropy)
{
    const float a = saturate(anisotropy);
    const float at = rough * rough * sqrt(2.0 / (1.0 + (1.0 - a) * (1.0 - a)));
    return float2(at, max((1.0 - a) * at, 1e-4));
}

// D in m's components: alpha_t alpha_b / (pi d^2), d = x^2 b/t + y^2 t/b + z^2 t b.
float ggxDAniso(float3 m, float2 a)
{
    const float d = m.x * m.x * (a.y / a.x) + m.y * m.y * (a.x / a.y) + m.z * m.z * a.x * a.y;
    return a.x * a.y / (kPbrPi * d * d);
}

float ggxLambdaAniso(float3 w, float2 a)
{
    const float2 aw = a * w.xy;
    return 0.5 * (sqrt(1.0 + dot(aw, aw) / (w.z * w.z)) - 1.0);
}

float ggxSingleAniso(float3 wo, float3 wi, float2 a)
{
    const float3 m = normalize(wo + wi);
    const float G2 = 1.0 / (1.0 + ggxLambdaAniso(wo, a) + ggxLambdaAniso(wi, a));
    return ggxDAniso(m, a) * G2 / (4.0 * wo.z);
}

// Bounded VNDF sampling with two alphas (Eto & Tokuyoshi 2023, the cap from the smaller).
float3 ggxSampleBoundedAniso(float3 wo, float2 alpha, float2 u)
{
    const float3 woStd = normalize(float3(wo.xy * alpha, wo.z));
    const float phi = (2.0 * u.x - 1.0) * kPbrPi;
    const float a = saturate(min(alpha.x, alpha.y));
    const float s = 1.0 + length(wo.xy);
    const float a2 = a * a, s2 = s * s;
    const float k = (1.0 - a2) * s2 / (s2 + a2 * wo.z * wo.z);
    const float b = wo.z > 0.0 ? k * woStd.z : woStd.z;
    const float z = (1.0 - u.y) * (1.0 + b) - b;
    const float sinTheta = sqrt(saturate(1.0 - z * z));
    const float3 mStd = float3(sinTheta * cos(phi), sinTheta * sin(phi), z) + woStd;
    return normalize(float3(mStd.xy * alpha, mStd.z));
}

float ggxPdfBoundedAniso(float3 wo, float3 wi, float2 alpha)
{
    const float3 m = normalize(wo + wi);
    const float ndf = ggxDAniso(m, alpha);
    const float2 ao = alpha * wo.xy;
    const float len2 = dot(ao, ao);
    const float t = sqrt(len2 + wo.z * wo.z);
    if (wo.z >= 0.0)
    {
        const float a = saturate(min(alpha.x, alpha.y));
        const float s = 1.0 + length(wo.xy);
        const float sa2 = a * a, s2 = s * s;
        const float k = (1.0 - sa2) * s2 / (s2 + sa2 * wo.z * wo.z);
        return ndf / (2.0 * (k * wo.z + t));
    }
    return ndf * (t - wo.z) / (2.0 * len2);
}

// ---- Fresnel --------------------------------------------------------------------------------

// The exact Fresnel reflectance of an unpolarised ray at cosine c onto a dielectric of relative
// IOR eta (transmitted over incident); 1 past total internal reflection.
float fresnelDielectric(float c, float eta)
{
    const float g2 = eta * eta - 1.0 + c * c;
    if (g2 < 0.0) return 1.0;
    const float g = sqrt(g2);
    const float a = (g - c) / (g + c);
    const float b = (c * (g + c) - 1.0) / (c * (g - c) + 1.0);
    return 0.5 * a * a * (1.0 + b * b);
}

// OpenPBR's specular_weight on a dielectric: F0 of `ior` scaled by `weight` (clamped so it
// stays at most 1), returned as the IOR that has that F0, on the same side of 1.
float modulatedIor(float ior, float weight)
{
    const float r = (1.0 - ior) / (1.0 + ior);
    const float F0 = r * r;
    if (!(F0 > 0.0)) return 1.0;
    const float xi = clamp(weight, 0.0, 1.0 / F0);
    const float eps = (ior >= 1.0 ? 1.0 : -1.0) * sqrt(xi * F0);
    return (1.0 + eps) / max(1.0 - eps, 1e-6);
}

static const float kF82Mu = 1.0 / 7.0;

// F82-tint's correction coefficient b: F(mu) = Schlick(mu) - b mu (1 - mu)^6, F(1/7) = tint *
// Schlick(1/7).
float3 f82B(float3 F0, float3 tint)
{
    const float m = 1.0 - kF82Mu;
    const float3 schlick = F0 + (1.0 - F0) * pow(m, 5.0);
    return schlick * (1.0 - tint) / (kF82Mu * pow(m, 6.0));
}

float3 fresnelF82(float c, float3 F0, float3 b)
{
    const float m = 1.0 - saturate(c);
    const float m5 = m * m * m * m * m;
    return saturate(F0 + (1.0 - F0) * m5 - b * (c * m5 * m));
}

// Kulla-Conty's multiple-scattering factor for a lobe whose Fresnel averages favg.
float3 msFresnel(float3 favg, float e1Avg)
{
    return favg * favg * e1Avg / (1.0 - favg * (1.0 - e1Avg));
}

// ---- EON (Portsmouth, Kutz & Hill, "EON: A practical energy-preserving rough diffuse BRDF",
// JCGT 2025). Ported from the authors' listings, github.com/portsmouth/EON-diffuse, MIT licence
// (LICENSE-EON.md beside this file),
// Copyright (c) 2024 Jamie Portsmouth. ----------------------------------------------------------

static const float kEonC1 = 0.5 - 2.0 / (3.0 * kPbrPi);
static const float kEonC2 = 2.0 / 3.0 - 28.0 / (15.0 * kPbrPi);

// FON's directional albedo (analytic).
float eonFonAlbedo(float mu, float r)
{
    const float AF = 1.0 / (1.0 + kEonC1 * r);
    const float BF = r * AF;
    mu = clamp(mu, -1.0, 1.0);
    const float Si = sqrt(1.0 - mu * mu);
    const float G = Si * (acos(mu) - Si * mu)
                  + (2.0 / 3.0) * (Si * mu * (1.0 + Si + Si * Si) / (1.0 + Si) - Si);
    return AF + (BF * kPbrInvPi) * G;
}

// EON's BRDF (without the cosine).
float3 eonEval(float3 rho, float r, float3 wi, float3 wo)
{
    const float mui = wi.z, muo = wo.z;
    const float s = dot(wi, wo) - mui * muo;
    const float sovertF = s > 0.0 ? s / max(mui, muo) : s;
    const float AF = 1.0 / (1.0 + kEonC1 * r);
    const float3 fss = (rho * kPbrInvPi) * AF * (1.0 + r * sovertF);
    const float EFo = eonFonAlbedo(muo, r);
    const float EFi = eonFonAlbedo(mui, r);
    const float avgEF = AF * (1.0 + kEonC2 * r);
    const float3 rhoMs = (rho * rho) * avgEF / (1.0 - rho * (1.0 - avgEF));
    const float eps = 1.0e-7;
    const float3 fms = (rhoMs * kPbrInvPi) * max(eps, 1.0 - EFo) * max(eps, 1.0 - EFi) / max(eps, 1.0 - avgEF);
    return fss + fms;
}

// EON's directional albedo.
float3 eonAlbedo(float3 rho, float r, float mu)
{
    const float AF = 1.0 / (1.0 + kEonC1 * r);
    const float EF = eonFonAlbedo(mu, r);
    const float avgEF = AF * (1.0 + kEonC2 * r);
    const float3 rhoMs = (rho * rho) * avgEF / (1.0 - rho * (1.0 - avgEF));
    return rho * EF + rhoMs * (1.0 - EF);
}

void eonLtcCoeffs(float mu, float r, out float a, out float b, out float c, out float d)
{
    a = 1.0 + r * (0.303392 + (-0.518982 + 0.111709 * mu) * mu + (-0.276266 + 0.335918 * mu) * r);
    b = r * (-1.16407 + 1.15859 * mu + (0.150815 - 0.150105 * mu) * r) / (mu * mu * mu - 1.43545);
    c = 1.0 + r * (0.20013 + (-0.506373 + 0.261777 * mu) * mu);
    d = r * (0.540852 + (-1.01625 + 0.475392 * mu) * mu) / (-1.0743 + (0.0725628 + mu) * mu);
}

// The LTC frame about wo: x toward wo's azimuth, z the normal.
void eonBasis(float3 wo, out float3 X, out float3 Y)
{
    const float lenSqr = dot(wo.xy, wo.xy);
    X = lenSqr > 0.0 ? float3(wo.x, wo.y, 0.0) * rsqrt(lenSqr) : float3(1.0, 0.0, 0.0);
    Y = float3(-X.y, X.x, 0.0);
}

float4 eonCltcSample(float3 wo, float r, float u1, float u2)
{
    float a, b, c, d;
    eonLtcCoeffs(wo.z, r, a, b, c, d);
    const float R = sqrt(u1);
    const float phi = 2.0 * kPbrPi * u2;
    float x = R * cos(phi);
    const float y = R * sin(phi);
    const float vz = 1.0 / sqrt(d * d + 1.0);
    const float s = 0.5 * (1.0 + vz);
    x = -lerp(sqrt(1.0 - y * y), x, s);
    const float3 wh = float3(x, y, sqrt(max(1.0 - (x * x + y * y), 0.0)));
    const float pdfWh = wh.z / (kPbrPi * s);
    float3 wi = float3(a * wh.x + b * wh.z, c * wh.y, d * wh.x + wh.z);
    const float len = length(wi);
    const float detM = c * (a - b * d);
    const float pdfWi = pdfWh * len * len * len / detM;
    float3 X, Y;
    eonBasis(wo, X, Y);
    wi = normalize(X * wi.x + Y * wi.y + float3(0.0, 0.0, wi.z));
    return float4(wi, pdfWi);
}

float eonCltcPdf(float3 wo, float3 wiLocal, float r)
{
    float3 X, Y;
    eonBasis(wo, X, Y);
    const float3 wi = float3(dot(X, wiLocal), dot(Y, wiLocal), wiLocal.z);
    float a, b, c, d;
    eonLtcCoeffs(wo.z, r, a, b, c, d);
    const float detM = c * (a - b * d);
    const float3 wh = float3(c * (wi.x - b * wi.z), (a - b * d) * wi.y, -c * (d * wi.x - a * wi.z));
    const float lensq = dot(wh, wh);
    const float vz = 1.0 / sqrt(d * d + 1.0);
    const float s = 0.5 * (1.0 + vz);
    return detM * detM / (lensq * lensq) * max(wh.z, 0.0) / (kPbrPi * s);
}

float eonUniformShare(float mu, float r)
{
    return pow(r, 0.1) * (0.162925 + (-0.372058 + (0.538233 - 0.290822 * mu) * mu) * mu);
}

float4 eonSample(float3 wo, float r, float u1, float u2)
{
    const float Pu = eonUniformShare(wo.z, r);
    const float Pc = 1.0 - Pu;
    float3 wi;
    float pdfC;
    if (u1 <= Pu)
    {
        u1 = u1 / Pu;
        const float sinTheta = sqrt(1.0 - u1 * u1);
        const float phi = 2.0 * kPbrPi * u2;
        wi = float3(sinTheta * cos(phi), sinTheta * sin(phi), u1);
        pdfC = eonCltcPdf(wo, wi, r);
    }
    else
    {
        u1 = (u1 - Pu) / Pc;
        const float4 s = eonCltcSample(wo, r, u1, u2);
        wi = s.xyz;
        pdfC = s.w;
    }
    return float4(wi, Pu * (0.5 * kPbrInvPi) + Pc * pdfC);
}

float eonPdf(float3 wo, float3 wi, float r)
{
    const float Pu = eonUniformShare(wo.z, r);
    return Pu * (0.5 * kPbrInvPi) + (1.0 - Pu) * eonCltcPdf(wo, wi, r);
}

// ---- the base -------------------------------------------------------------------------------

// The step's parameters, resolved from textures (OpenPBR's names).
struct MaterialInputs
{
    float3 baseColor;           // base_color
    float baseWeight;           // base_weight
    float baseMetalness;        // base_metalness
    float baseDiffuseRoughness; // base_diffuse_roughness
    float specularWeight;       // specular_weight
    float3 specularColor;       // specular_color
    float specularRoughness;    // specular_roughness
    float specularIor;          // specular_ior
    float specularRoughnessAnisotropy;  // specular_roughness_anisotropy (along the frame's x)
    float coatWeight;           // coat_weight
    float3 coatColor;           // coat_color
    float coatRoughness;        // coat_roughness
    float coatRoughnessAnisotropy;      // coat_roughness_anisotropy
    float coatIor;              // coat_ior
    float coatDarkening;        // coat_darkening
    float fuzzWeight;           // fuzz_weight
    float3 fuzzColor;           // fuzz_color
    float fuzzRoughness;        // fuzz_roughness
};

MaterialInputs defaultMaterialInputs()
{
    MaterialInputs m;
    m.baseColor = float3(0.8);
    m.baseWeight = 1.0;
    m.baseMetalness = 0.0;
    m.baseDiffuseRoughness = 0.0;
    m.specularWeight = 1.0;
    m.specularColor = float3(1.0);
    m.specularRoughness = 0.3;
    m.specularIor = 1.5;
    m.specularRoughnessAnisotropy = 0.0;
    m.coatWeight = 0.0;
    m.coatColor = float3(1.0);
    m.coatRoughness = 0.0;
    m.coatRoughnessAnisotropy = 0.0;
    m.coatIor = 1.6;
    m.coatDarkening = 1.0;
    m.fuzzWeight = 0.0;
    m.fuzzColor = float3(1.0);
    m.fuzzRoughness = 0.5;
    return m;
}

float pbrLuminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// What sample() returns: the direction, f cos / pdf, the pdf (0: no sample) and the lobe it
// came from (0 the base's specular, 1 the diffuse, 2 the coat, 3 the fuzz).
struct BsdfSample
{
    float3 wi;
    float3 weight;
    float pdf;
    uint lobe;
};

struct OpenPBRBase
{
    float3 rho;          // the diffuse albedo, base_weight * base_color
    float diffRough;
    float metalness;
    float rough;         // specular roughness, clamped at kMinSpecularRoughness
    float alpha;         // rough^2
    float anisotropy;    // specular_roughness_anisotropy; 0: the isotropic lobe
    float2 alphaXY;      // (alpha_t, alpha_b) when anisotropic
    float tableRough;    // the roughness the albedos read the tables at: rough, or viewedFrom()'s
    float eta;           // the dielectric's IOR after specular_weight
    float3 specColor;
    float3 metalF0;      // specular_weight folded in below
    float3 metalB;
    float metalWeight;   // specular_weight on the metal

    __init(MaterialInputs m)
    {
        rho = saturate(m.baseWeight * m.baseColor);
        diffRough = saturate(m.baseDiffuseRoughness);
        metalness = saturate(m.baseMetalness);
        rough = clamp(m.specularRoughness, kMinSpecularRoughness, 1.0);
        alpha = rough * rough;
        anisotropy = saturate(m.specularRoughnessAnisotropy);
        alphaXY = anisotropy > 0.0 ? ggxAnisoAlpha(rough, anisotropy) : float2(alpha);
        tableRough = rough;
        eta = clamp(modulatedIor(m.specularIor, m.specularWeight), 1.0, kPbrEtaMax);
        specColor = saturate(m.specularColor);
        metalF0 = rho;
        metalB = f82B(metalF0, specColor);
        metalWeight = saturate(m.specularWeight);
    }

    // The metal's Fresnel average, F82's in closed form: F0 + (1 - F0) / 21 - b / 126.
    float3 metalFavg() { return metalWeight * (metalF0 + (1.0 - metalF0) / 21.0 - metalB / 126.0); }

    // The multiple-scattering lobe with F = 1 (x cos(wi)).
    float msLobe(float3 wo, float3 wi, OpenPBRTables t)
    {
        const float e1Avg = pbrE1Avg(t, rough, anisotropy);
        const float ro = pbrTableRough(t, rough, anisotropy, wo), ri = pbrTableRough(t, rough, anisotropy, wi);
        return (1.0 - t.E1(wo.z, ro)) * (1.0 - t.E1(wi.z, ri)) / (kPbrPi * max(1.0 - e1Avg, 1e-6)) * wi.z;
    }

    // The dielectric specular's albedo toward wo (untinted: the albedo scaling's E).
    float dielectricAlbedo(float mu, OpenPBRTables t)
    {
        const float ms = msFresnel(float3(t.FavgD(eta)), pbrE1Avg(t, rough, anisotropy)).x;
        return t.EssD(mu, tableRough, eta) + ms * (1.0 - t.E1(mu, tableRough));
    }

    float3 metalAlbedo(float mu, OpenPBRTables t)
    {
        const float e1 = t.E1(mu, tableRough);
        const float3 ess = metalWeight * (metalF0 * e1 + (1.0 - metalF0) * t.S5(mu, tableRough) - metalB * t.S6(mu, tableRough));
        return ess + msFresnel(metalFavg(), pbrE1Avg(t, rough, anisotropy)) * (1.0 - e1);
    }

    // The specular lobes (the metal's and the dielectric's, tinted) and the diffuse beneath them,
    // each x cos(wi): eval() is their sum. The deferred frame lights a sphere or a rectangle from a
    // different direction for each (core/deferred_lighting.slang).
    void evalLobes(float3 wo, float3 wi, OpenPBRTables t, out float3 specular, out float3 diffuse)
    {
        specular = float3(0.0);
        diffuse = float3(0.0);
        if (wo.z <= 0.0 || wi.z <= 0.0) return;
        const float3 m = normalize(wo + wi);
        const float c = dot(wo, m);
        const float ss = anisotropy > 0.0 ? ggxSingleAniso(wo, wi, alphaXY) : ggxSingle(wo, wi, alpha * alpha);
        const float ms = msLobe(wo, wi, t);
        const float e1Avg = pbrE1Avg(t, rough, anisotropy);
        if (metalness > 0.0)
        {
            const float3 F = metalWeight * fresnelF82(c, metalF0, metalB);
            specular += metalness * (F * ss + msFresnel(metalFavg(), e1Avg) * ms);
        }
        if (metalness < 1.0)
        {
            const float spec = fresnelDielectric(c, eta) * ss + msFresnel(float3(t.FavgD(eta)), e1Avg).x * ms;
            specular += (1.0 - metalness) * specColor * spec;
            diffuse = (1.0 - metalness) * (1.0 - dielectricAlbedo(wo.z, t)) * eonEval(rho, diffRough, wi, wo) * wi.z;
        }
    }

    // BSDF x cos(wi).
    float3 eval(float3 wo, float3 wi, OpenPBRTables t)
    {
        float3 specular, diffuse;
        evalLobes(wo, wi, t, specular, diffuse);
        return specular + diffuse;
    }

    // This surface with its specular lobe as rough as `r` (a light's size widening it).
    OpenPBRBase withRoughness(float r)
    {
        OpenPBRBase b = this;
        b.rough = clamp(r, kMinSpecularRoughness, 1.0);
        b.alpha = b.rough * b.rough;
        b.alphaXY = b.anisotropy > 0.0 ? ggxAnisoAlpha(b.rough, b.anisotropy) : float2(b.alpha);
        b.tableRough = b.rough;
        return b;
    }

    // This surface's albedos as seen from wo: an anisotropic lobe's are read from the isotropic
    // tables at the roughness whose E1 is its own there (OpenPBRTables.AnisoRough). Isotropic: this
    // surface unchanged.
    OpenPBRBase viewedFrom(float3 wo, OpenPBRTables t)
    {
        OpenPBRBase b = this;
        b.tableRough = pbrTableRough(t, rough, anisotropy, wo);
        return b;
    }

    // The specular lobes' directional albedo toward wo, and the diffuse's beneath them:
    // albedo() is their sum. The deferred frame weighs the environment's reflection by the first
    // (the split sum) and its diffuse light by the second.
    float3 specularAlbedo(float3 wo, OpenPBRTables t)
    {
        if (wo.z <= 0.0) return float3(0.0);
        return lerp(specColor * dielectricAlbedo(wo.z, t), metalAlbedo(wo.z, t), metalness);
    }

    float3 diffuseAlbedo(float3 wo, OpenPBRTables t)
    {
        if (wo.z <= 0.0) return float3(0.0);
        return (1.0 - metalness) * (1.0 - dielectricAlbedo(wo.z, t)) * eonAlbedo(rho, diffRough, wo.z);
    }

    float3 albedo(float3 wo, OpenPBRTables t)
    {
        return specularAlbedo(wo, t) + diffuseAlbedo(wo, t);
    }

    // The chance of sampling the specular lobe (GGX, the metal and the dielectric's together):
    // its share of the albedo's luminance.
    float specularShare(float3 wo, OpenPBRTables t)
    {
        const float ed = dielectricAlbedo(wo.z, t);
        const float spec = metalness * pbrLuminance(metalAlbedo(wo.z, t)) + (1.0 - metalness) * pbrLuminance(specColor) * ed;
        const float diff = (1.0 - metalness) * (1.0 - ed) * pbrLuminance(eonAlbedo(rho, diffRough, wo.z));
        const float total = spec + diff;
        return total > 0.0 ? spec / total : 1.0;
    }

    // The specular lobe's sampling, isotropic or not.
    float3 sampleSpecular(float3 wo, float2 u)
    {
        return anisotropy > 0.0 ? ggxSampleBoundedAniso(wo, alphaXY, u) : ggxSampleBounded(wo, alpha, u);
    }

    float pdfSpecular(float3 wo, float3 wi)
    {
        return anisotropy > 0.0 ? ggxPdfBoundedAniso(wo, wi, alphaXY) : ggxPdfBounded(wo, wi, alpha);
    }

    float pdf(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;
        const float ps = specularShare(wo, t);
        return ps * pdfSpecular(wo, wi) + (1.0 - ps) * eonPdf(wo, wi, diffRough);
    }

    BsdfSample sample(float3 wo, float3 u, OpenPBRTables t)
    {
        BsdfSample s;
        s.wi = float3(0.0, 0.0, 1.0);
        s.weight = float3(0.0);
        s.pdf = 0.0;
        s.lobe = 0u;
        if (wo.z <= 0.0) return s;
        const float ps = specularShare(wo, t);
        if (u.x < ps)
        {
            const float3 m = sampleSpecular(wo, u.yz);
            s.wi = reflect(-wo, m);
        }
        else
        {
            s.wi = eonSample(wo, diffRough, u.y, u.z).xyz;
            s.lobe = 1u;
        }
        if (s.wi.z <= 0.0) return s;
        s.pdf = ps * pdfSpecular(wo, s.wi) + (1.0 - ps) * eonPdf(wo, s.wi, diffRough);
        if (s.pdf > 0.0) s.weight = eval(wo, s.wi, t) / s.pdf;
        return s;
    }
};

// ---- the coat -------------------------------------------------------------------------------

// The coat's lobe: a white GGX dielectric of IOR coat_ior (OpenPBR's coat reflects untinted; its
// colour is the medium's absorption, applied to what lies below), energy-compensated as the base's
// dielectric is.
struct OpenPBRCoat
{
    float rough;
    float alpha;
    float anisotropy;
    float2 alphaXY;
    float eta;

    __init(float roughness, float roughnessAnisotropy, float ior)
    {
        rough = clamp(roughness, kMinSpecularRoughness, 1.0);
        alpha = rough * rough;
        anisotropy = saturate(roughnessAnisotropy);
        alphaXY = anisotropy > 0.0 ? ggxAnisoAlpha(rough, anisotropy) : float2(alpha);
        eta = clamp(ior, 1.0, kPbrEtaMax);
    }

    float albedo(float3 wo, OpenPBRTables t)
    {
        const float r = pbrTableRough(t, rough, anisotropy, wo);
        const float ms = msFresnel(float3(t.FavgD(eta)), pbrE1Avg(t, rough, anisotropy)).x;
        return t.EssD(wo.z, r, eta) + ms * (1.0 - t.E1(wo.z, r));
    }

    // BSDF x cos(wi).
    float eval(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;
        const float3 m = normalize(wo + wi);
        const float ss = anisotropy > 0.0 ? ggxSingleAniso(wo, wi, alphaXY) : ggxSingle(wo, wi, alpha * alpha);
        const float e1Avg = pbrE1Avg(t, rough, anisotropy);
        const float ro = pbrTableRough(t, rough, anisotropy, wo), ri = pbrTableRough(t, rough, anisotropy, wi);
        const float ms = (1.0 - t.E1(wo.z, ro)) * (1.0 - t.E1(wi.z, ri)) / (kPbrPi * max(1.0 - e1Avg, 1e-6)) * wi.z;
        return fresnelDielectric(dot(wo, m), eta) * ss + msFresnel(float3(t.FavgD(eta)), e1Avg).x * ms;
    }

    float3 sampleNormal(float3 wo, float2 u)
    {
        return anisotropy > 0.0 ? ggxSampleBoundedAniso(wo, alphaXY, u) : ggxSampleBounded(wo, alpha, u);
    }

    // This lobe as rough as `r` (a light's size widening it).
    OpenPBRCoat withRoughness(float r)
    {
        OpenPBRCoat c = this;
        c.rough = clamp(r, kMinSpecularRoughness, 1.0);
        c.alpha = c.rough * c.rough;
        c.alphaXY = c.anisotropy > 0.0 ? ggxAnisoAlpha(c.rough, c.anisotropy) : float2(c.alpha);
        return c;
    }

    float pdf(float3 wo, float3 wi)
    {
        return anisotropy > 0.0 ? ggxPdfBoundedAniso(wo, wi, alphaXY) : ggxPdfBounded(wo, wi, alpha);
    }
};

// ---- the fuzz -------------------------------------------------------------------------------
// Zeltner, Burley & Chiang, "Practical Multiple-Scattering Sheen Using Linearly Transformed
// Cosines" (SIGGRAPH Talks 2022), OpenPBR's fuzz. The fits of its directional albedo and of its
// LTC's inverse matrix are MaterialX's (libraries/pbrlib/genglsl/lib/mx_microfacet_sheen.glsl,
// adapted there from github.com/tizian/ltc-sheen), Apache License 2.0, Copyright Contributors to
// the MaterialX Project (LICENSE-MaterialX.md beside this file). x is cos(wo), y the roughness.

float fuzzAlbedoFit(float x, float y)
{
    const float s = y * (0.0206607 + 1.58491 * y) / (0.0379424 + y * (1.32227 + y));
    const float m = y * (-0.193854 + y * (-1.14885 + y * (1.7932 - 0.95943 * y * y))) / (0.046391 + y);
    const float o = y * (0.000654023 + (-0.0207818 + 0.119681 * y) * y) / (1.26264 + y * (-1.92021 + y));
    const float q = (x - m) / s;
    return saturate(exp(-0.5 * q * q) / (s * sqrt(2.0 * kPbrPi)) + o);
}

float fuzzLtcAInv(float x, float y)
{
    return (2.58126 * x + 0.813703 * y) * y / (1.0 + 0.310327 * x * x + 2.60994 * x * y);
}

float fuzzLtcBInv(float x, float y)
{
    return sqrt(1.0 - x) * (y - 1.0) * y * y * y / (0.0000254053 + 1.71228 * x - 1.71506 * x * y + 1.34174 * y * y);
}

struct OpenPBRFuzz
{
    float rough;   // fuzz_roughness, held in the fits' [0.01, 1]

    __init(float roughness) { rough = clamp(roughness, 0.01, 1.0); }

    // The untinted directional albedo, E_fuzz.
    float albedo(float mu) { return fuzzAlbedoFit(saturate(mu), rough); }

    // The LTC's distribution of wi (normalised over the upper hemisphere, which the transform
    // maps onto itself): the clamped cosine at M^-1 wi, by the Jacobian (aInv / |M^-1 wi|^2)^2.
    float D(float3 wo, float3 wi)
    {
        if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;
        float3 X, Y;
        eonBasis(wo, X, Y);
        const float3 w = float3(dot(wi, X), dot(wi, Y), wi.z);
        const float aInv = fuzzLtcAInv(saturate(wo.z), rough);
        const float bInv = fuzzLtcBInv(saturate(wo.z), rough);
        const float3 o = float3(aInv * w.x + bInv * w.z, aInv * w.y, w.z);
        const float len2 = dot(o, o);
        const float j = aInv / len2;
        return o.z * kPbrInvPi * j * j;
    }

    // A direction drawn with density D: a cosine sample transformed by M.
    float3 sample(float3 wo, float2 u)
    {
        const float r = sqrt(u.x);
        const float phi = 2.0 * kPbrPi * u.y;
        const float3 c = float3(r * cos(phi), r * sin(phi), sqrt(saturate(1.0 - u.x)));
        const float aInv = fuzzLtcAInv(saturate(wo.z), rough);
        const float bInv = fuzzLtcBInv(saturate(wo.z), rough);
        const float3 w = normalize(float3((c.x - c.z * bInv) / aInv, c.y / aInv, c.z));
        float3 X, Y;
        eonBasis(wo, X, Y);
        return w.x * X + w.y * Y + float3(0.0, 0.0, w.z);
    }
};

// ---- the surface ----------------------------------------------------------------------------

// OpenPBR's whole as step 2 builds it: fuzz over coat over the base,
//   f = F f_fuzz + (1 - F E_fuzz(wo)) [C f_coat + lerp(1, T (1 - E_coat(wo)), C) lerp(1, Delta, C delta) f_base']
// with f_fuzz cos = fuzz_color E_fuzz D, T the coat's absorption along wi and wo, Delta its
// darkening, and f_base' the base seen through the coat (its IOR relative to the coat's, its
// roughness widened by the coat's). A surface with neither coat nor fuzz is its base, through the
// base's own code (so the frame of a step-1 material is the same bits).
struct OpenPBRSurface
{
    OpenPBRBase base;
    OpenPBRCoat coat;
    OpenPBRFuzz fuzz;
    float coatWeight;
    float3 coatColor;
    float coatDarkening;
    float fuzzWeight;
    float3 fuzzColor;
    bool layered;

    __init(MaterialInputs m)
    {
        coatWeight = saturate(m.coatWeight);
        coatColor = saturate(m.coatColor);
        coatDarkening = saturate(m.coatDarkening);
        fuzzWeight = saturate(m.fuzzWeight);
        fuzzColor = saturate(m.fuzzColor);
        layered = coatWeight > 0.0 || fuzzWeight > 0.0;
        coat = OpenPBRCoat(m.coatRoughness, m.coatRoughnessAnisotropy, m.coatIor);
        fuzz = OpenPBRFuzz(m.fuzzRoughness);
        MaterialInputs b = m;
        if (coatWeight > 0.0)
        {
            // The specular IOR relative to the coat's medium (inverted past TIR), and the
            // roughness the coat's scattering adds.
            const float nb = max(m.specularIor, 1e-3), nc = max(m.coatIor, 1e-3);
            b.specularIor = lerp(nb, nc > nb ? nc / nb : nb / nc, coatWeight);
            const float rb = saturate(m.specularRoughness), rc = saturate(m.coatRoughness);
            const float widened = pow(min(1.0, rb * rb * rb * rb + 2.0 * rc * rc * rc * rc), 0.25);
            b.specularRoughness = lerp(rb, widened, coatWeight);
        }
        base = OpenPBRBase(b);
    }

    // The base as the tables see it from wo (OpenPBRBase.viewedFrom).
    OpenPBRBase baseAt(float3 wo, OpenPBRTables t) { return base.viewedFrom(wo, t); }

    // 1 / mu_t: the path length through the coat of a ray at cosine mu outside it, refracted.
    float coatPath(float mu)
    {
        return 1.0 / max(sqrt(saturate(1.0 - (1.0 - mu * mu) / (coat.eta * coat.eta))), 1e-4);
    }

    // coatPath's mean over cosine-distributed directions, 2 eta (eta - sqrt(eta^2 - 1)): what a
    // diffuse lobe's light crosses on its way up, on average.
    float coatPathDiffuse()
    {
        return 2.0 * coat.eta * (coat.eta - sqrt(coat.eta * coat.eta - 1.0));
    }

    // The coat's absorption along paths p_i and p_o: coat_color at normal incidence there and back,
    // coat_color^((p_i + p_o) / 2).
    float3 coatTransmittance(float pathI, float pathO)
    {
        return pow(max(coatColor, float3(1e-6)), 0.5 * (pathI + pathO));
    }

    // lerp(1, Delta, C delta): the base darkened by light the coat reflects back into it.
    float3 coatDarken(float muO, OpenPBRTables t)
    {
        if (coatDarkening <= 0.0) return float3(1.0);
        const float3 Eb = baseAt(float3(0.0, 0.0, 1.0), t).albedo(float3(0.0, 0.0, 1.0), t);
        const float Kr = 1.0 - (1.0 - t.FavgD(coat.eta)) / (coat.eta * coat.eta);
        const float Ks = fresnelDielectric(muO, coat.eta);
        const float r0 = (base.eta - 1.0) / (base.eta + 1.0);
        const float rd = lerp(1.0, base.rough, r0 * r0);
        const float rb = lerp(rd, base.rough, base.metalness);
        const float K = lerp(Ks, Kr, rb);
        const float3 delta = (1.0 - K) / max(1.0 - Eb * K, float3(1e-6));
        return lerp(float3(1.0), delta, coatWeight * coatDarkening);
    }

    // What reaches the base through the coat along a path pathI in and out at wo, and through the
    // fuzz at wo.
    float3 baseScale(float pathI, float3 wo, OpenPBRTables t)
    {
        float3 k = float3(1.0 - fuzzWeight * fuzz.albedo(wo.z));
        if (coatWeight > 0.0)
            k *= lerp(float3(1.0), coatTransmittance(pathI, coatPath(wo.z)) * (1.0 - coat.albedo(wo, t)), coatWeight)
               * coatDarken(wo.z, t);
        return k;
    }

    // The lobes x cos(wi), split as RT direct light demodulates them: the specular (the coat's and
    // the base's) and the diffuse (the base's and the fuzz).
    void evalLobes(float3 wo, float3 wi, OpenPBRTables t, out float3 specular, out float3 diffuse)
    {
        baseAt(wo, t).evalLobes(wo, wi, t, specular, diffuse);
        if (!layered || wo.z <= 0.0 || wi.z <= 0.0) return;
        const float3 k = baseScale(coatPath(wi.z), wo, t);
        const float fuzzOver = 1.0 - fuzzWeight * fuzz.albedo(wo.z);
        specular = k * specular + fuzzOver * coatWeight * coat.eval(wo, wi, t);
        diffuse = k * diffuse + fuzzWeight * fuzzColor * fuzz.albedo(wo.z) * fuzz.D(wo, wi);
    }

    float3 eval(float3 wo, float3 wi, OpenPBRTables t)
    {
        float3 specular, diffuse;
        evalLobes(wo, wi, t, specular, diffuse);
        return specular + diffuse;
    }

    // The directional albedos: the specular's light crosses the coat about as it came (wi near the
    // mirror of wo), the diffuse's along coatPathDiffuse.
    float3 specularAlbedo(float3 wo, OpenPBRTables t)
    {
        const float3 s = baseAt(wo, t).specularAlbedo(wo, t);
        if (!layered || wo.z <= 0.0) return s;
        const float fuzzOver = 1.0 - fuzzWeight * fuzz.albedo(wo.z);
        return baseScale(coatPath(wo.z), wo, t) * s + fuzzOver * coatWeight * coat.albedo(wo, t);
    }

    float3 diffuseAlbedo(float3 wo, OpenPBRTables t)
    {
        const float3 d = baseAt(wo, t).diffuseAlbedo(wo, t);
        if (!layered || wo.z <= 0.0) return d;
        return baseScale(coatPathDiffuse(), wo, t) * d + fuzzWeight * fuzzColor * fuzz.albedo(wo.z);
    }

    float3 albedo(float3 wo, OpenPBRTables t)
    {
        return specularAlbedo(wo, t) + diffuseAlbedo(wo, t);
    }

    // The coat's part of specularAlbedo (the deferred frame's split sum reads it at the coat's
    // roughness, the rest at the base's).
    float coatAlbedo(float3 wo, OpenPBRTables t)
    {
        if (coatWeight <= 0.0 || wo.z <= 0.0) return 0.0;
        return (1.0 - fuzzWeight * fuzz.albedo(wo.z)) * coatWeight * coat.albedo(wo, t);
    }

    // This surface with each specular lobe's alpha widened by `da` (a sphere's or a rectangle's
    // angular size, Karis 2013), each lobe from its own roughness.
    OpenPBRSurface widened(float da)
    {
        OpenPBRSurface w = this;
        w.base = base.withRoughness(sqrt(clamp(base.alpha + da, 0.0, 1.0)));
        if (coatWeight > 0.0) w.coat = coat.withRoughness(sqrt(clamp(coat.alpha + da, 0.0, 1.0)));
        return w;
    }

    // The chance of drawing each lobe, by the luminance of its albedo: (base specular, diffuse,
    // coat, fuzz).
    float4 lobeShares(float3 wo, OpenPBRTables t)
    {
        float4 w;
        const OpenPBRBase b = baseAt(wo, t);
        w.x = pbrLuminance(baseScale(coatPath(wo.z), wo, t) * b.specularAlbedo(wo, t));
        w.y = pbrLuminance(baseScale(coatPathDiffuse(), wo, t) * b.diffuseAlbedo(wo, t));
        w.z = (1.0 - fuzzWeight * fuzz.albedo(wo.z)) * coatWeight * coat.albedo(wo, t);
        w.w = fuzzWeight * pbrLuminance(fuzzColor) * fuzz.albedo(wo.z);
        const float total = w.x + w.y + w.z + w.w;
        return total > 0.0 ? w / total : float4(1.0, 0.0, 0.0, 0.0);
    }

    float pdfOf(float3 wo, float3 wi, float4 p)
    {
        return p.x * base.pdfSpecular(wo, wi) + p.y * eonPdf(wo, wi, base.diffRough) + p.z * coat.pdf(wo, wi)
             + p.w * fuzz.D(wo, wi);
    }

    float pdf(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (!layered) return baseAt(wo, t).pdf(wo, wi, t);
        if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;
        return pdfOf(wo, wi, lobeShares(wo, t));
    }

    BsdfSample sample(float3 wo, float3 u, OpenPBRTables t)
    {
        if (!layered) return baseAt(wo, t).sample(wo, u, t);
        BsdfSample s;
        s.wi = float3(0.0, 0.0, 1.0);
        s.weight = float3(0.0);
        s.pdf = 0.0;
        s.lobe = 0u;
        if (wo.z <= 0.0) return s;
        const float4 p = lobeShares(wo, t);
        if (u.x < p.x)
        {
            s.wi = reflect(-wo, base.sampleSpecular(wo, u.yz));
        }
        else if (u.x < p.x + p.y)
        {
            s.wi = eonSample(wo, base.diffRough, u.y, u.z).xyz;
            s.lobe = 1u;
        }
        else if (u.x < p.x + p.y + p.z)
        {
            s.wi = reflect(-wo, coat.sampleNormal(wo, u.yz));
            s.lobe = 2u;
        }
        else
        {
            s.wi = fuzz.sample(wo, u.yz);
            s.lobe = 3u;
        }
        if (s.wi.z <= 0.0) return s;
        s.pdf = pdfOf(wo, s.wi, p);
        if (s.pdf > 0.0) s.weight = eval(wo, s.wi, t) / s.pdf;
        return s;
    }
};
