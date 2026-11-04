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
static const uint kPbrOffAnisoS56 = kPbrOffAnisoE1Avg + kPbrAnisoA * kPbrAnisoRough;   // AnisoRough's grid x 2
static const uint kPbrOffEssT = kPbrOffAnisoS56 + 2u * kPbrAnisoA * kPbrAnisoRough * kPbrAnisoMu * kPbrAnisoPhi;
// Step 3 (transmission), each [n][rough][mu] on EssD's mu and roughness axes with 32 points of n over
// [1, kPbrEtaMax] (16 left glass at IOR 1.33, between two, 1.1% dark seen from inside): the
// refraction's single-scatter albedo entering (EssT, relative IOR n), and leaving (relative 1 / n)
// the reflection's (EssDIn) and the refraction's (EssTIn).
static const uint kPbrGlassEta = 32u;
static const uint kPbrOffEssDIn = kPbrOffEssT + kPbrDielMu * kPbrDielRough * kPbrGlassEta;
static const uint kPbrOffEssTIn = kPbrOffEssDIn + kPbrDielMu * kPbrDielRough * kPbrGlassEta;
// Area lights by linearly transformed cosines (Heitz, Dupuy, Hill & Neubelt 2016): [rough][u][4], u
// = sqrt(1 - mu), the normalized inverse matrix fitted to GGX single scatter (F = 1).
static const uint kPbrLtcN = 32u;
static const uint kPbrOffLtc = kPbrOffEssTIn + kPbrDielMu * kPbrDielRough * kPbrGlassEta;
static const uint kPbrTableFloats = kPbrOffLtc + kPbrLtcN * kPbrLtcN * 4u;
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

    // A [n][rough][mu] table at offset off, n the IOR in [1, kPbrEtaMax] over kPbrGlassEta points (step 3's
    // EssT, EssDIn, EssTIn), trilinear.
    float glass(uint off, float mu, float rough, float n)
    {
        uint im, ir, ie;
        float fm, fr, fe;
        pbrGridCoord(sqrt(saturate(mu)), kPbrDielMu, im, fm);
        pbrGridCoord(rough, kPbrDielRough, ir, fr);
        pbrGridCoord((n - 1.0) / (kPbrEtaMax - 1.0), kPbrGlassEta, ie, fe);
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
    float EssT(float mu, float rough, float n) { return glass(kPbrOffEssT, mu, rough, n); }
    float EssDIn(float mu, float rough, float n) { return glass(kPbrOffEssDIn, mu, rough, n); }
    float EssTIn(float mu, float rough, float n) { return glass(kPbrOffEssTIn, mu, rough, n); }

    // The LTC's inverse matrix (a, b, c, d: rows (a, 0, b), (0, 1, 0), (c, 0, d)) at the view's
    // cosine mu and the lobe's roughness, bilinear.
    float4 Ltc(float mu, float rough)
    {
        uint iu, ir;
        float fu, fr;
        pbrGridCoord(sqrt(saturate(1.0 - mu)), kPbrLtcN, iu, fu);
        pbrGridCoord(rough, kPbrLtcN, ir, fr);
        float4 v[2];
        [unroll]
        for (uint k = 0u; k < 2u; ++k)
        {
            const uint r0 = kPbrOffLtc + ((ir + k) * kPbrLtcN + iu) * 4u;
            const float4 a = float4(data[r0], data[r0 + 1u], data[r0 + 2u], data[r0 + 3u]);
            const float4 b = float4(data[r0 + 4u], data[r0 + 5u], data[r0 + 6u], data[r0 + 7u]);
            v[k] = lerp(a, b, fu);
        }
        return lerp(v[0], v[1], fr);
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

    // The anisotropic lobe's own S5 and S6 seen from w, on AnisoRough's grid.
    float2 AnisoS56(float aniso, float rough, float3 w)
    {
        uint ia, ir, im, ip;
        float fa, fr, fm, fp;
        pbrGridCoord(1.0 - sqrt(saturate(1.0 - aniso)), kPbrAnisoA, ia, fa);
        pbrGridCoord(rough, kPbrAnisoRough, ir, fr);
        pbrGridCoord(sqrt(saturate(w.z)), kPbrAnisoMu, im, fm);
        pbrGridCoord(atan2(abs(w.y), abs(w.x)) * (2.0 / kPbrPi), kPbrAnisoPhi, ip, fp);
        float2 v = float2(0.0);
        [unroll]
        for (uint k = 0u; k < 16u; ++k)
        {
            const uint da = k & 1u, dr = (k >> 1u) & 1u, dm = (k >> 2u) & 1u, dp = k >> 3u;
            const float w4 = (da == 1u ? fa : 1.0 - fa) * (dr == 1u ? fr : 1.0 - fr) * (dm == 1u ? fm : 1.0 - fm)
                           * (dp == 1u ? fp : 1.0 - fp);
            const uint i = 2u * ((((ia + da) * kPbrAnisoRough + ir + dr) * kPbrAnisoMu + im + dm) * kPbrAnisoPhi + ip + dp);
            v += w4 * float2(data[kPbrOffAnisoS56 + i], data[kPbrOffAnisoS56 + i + 1u]);
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

// What an anisotropic lobe's S5 and S6 seen from w are over the isotropic tables' at its table
// roughness r: E1 matches there, the Fresnel-weighted moments do not (a coloured metal read 20%
// high at 81 degrees, a coat 46%). 0 for an isotropic lobe.
float2 pbrS56Delta(OpenPBRTables t, float rough, float anisotropy, float3 w, float r)
{
    if (!(anisotropy > 0.0)) return float2(0.0);
    return t.AnisoS56(anisotropy, rough, w) - float2(t.S5(w.z, r), t.S6(w.z, r));
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

// ---- area lights by linearly transformed cosines ---------------------------------------------
// Heitz, Dupuy, Hill & Neubelt 2016 (the polygon clipped to the horizon) with Heitz's 2017 edge
// integral. ltcQuad: the clamped cosine's integral over a quad whose corners are given in a frame
// (z the normal), after the LTC's inverse matrix m (OpenPBRTables.Ltc): the fraction of a GGX lobe
// the quad covers. A white quad over the whole upper hemisphere gives 1.

float3 ltcEdge(float3 v1, float3 v2)
{
    const float x = dot(v1, v2);
    const float y = abs(x);
    const float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    const float b = 3.4175940 + (4.1616724 + y) * y;
    const float v = a / b;
    const float ts = x > 0.0 ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * ts;
}

// The polygon L[0..3] clipped to z >= 0, in place; its corner count (0, 3, 4 or 5).
uint ltcClip(inout float3 L[5])
{
    uint config = 0u;
    if (L[0].z > 0.0) config += 1u;
    if (L[1].z > 0.0) config += 2u;
    if (L[2].z > 0.0) config += 4u;
    if (L[3].z > 0.0) config += 8u;
    uint n = 0u;
    if (config == 1u) { n = 3u; L[1] = -L[1].z * L[0] + L[0].z * L[1]; L[2] = -L[3].z * L[0] + L[0].z * L[3]; }
    else if (config == 2u) { n = 3u; L[0] = -L[0].z * L[1] + L[1].z * L[0]; L[2] = -L[2].z * L[1] + L[1].z * L[2]; }
    else if (config == 3u) { n = 4u; L[2] = -L[2].z * L[1] + L[1].z * L[2]; L[3] = -L[3].z * L[0] + L[0].z * L[3]; }
    else if (config == 4u) { n = 3u; L[0] = -L[3].z * L[2] + L[2].z * L[3]; L[1] = -L[1].z * L[2] + L[2].z * L[1]; }
    else if (config == 6u) { n = 4u; L[0] = -L[0].z * L[1] + L[1].z * L[0]; L[3] = -L[3].z * L[2] + L[2].z * L[3]; }
    else if (config == 7u) { n = 5u; L[4] = -L[3].z * L[0] + L[0].z * L[3]; L[3] = -L[3].z * L[2] + L[2].z * L[3]; }
    else if (config == 8u) { n = 3u; L[0] = -L[0].z * L[3] + L[3].z * L[0]; L[1] = -L[2].z * L[3] + L[3].z * L[2]; L[2] = L[3]; }
    else if (config == 9u) { n = 4u; L[1] = -L[1].z * L[0] + L[0].z * L[1]; L[2] = -L[2].z * L[3] + L[3].z * L[2]; }
    else if (config == 11u) { n = 5u; L[4] = L[3]; L[3] = -L[2].z * L[3] + L[3].z * L[2]; L[2] = -L[2].z * L[1] + L[1].z * L[2]; }
    else if (config == 12u) { n = 4u; L[1] = -L[1].z * L[2] + L[2].z * L[1]; L[0] = -L[0].z * L[3] + L[3].z * L[0]; }
    else if (config == 13u) { n = 5u; L[4] = L[3]; L[3] = L[2]; L[2] = -L[1].z * L[2] + L[2].z * L[1]; L[1] = -L[1].z * L[0] + L[0].z * L[1]; }
    else if (config == 14u) { n = 5u; L[4] = -L[0].z * L[3] + L[3].z * L[0]; L[0] = -L[0].z * L[1] + L[1].z * L[0]; }
    else if (config == 15u) { n = 4u; }
    if (n == 3u) L[3] = L[0];
    if (n == 4u) L[4] = L[0];
    return n;
}

float ltcQuad(float4 m, float3 q0, float3 q1, float3 q2, float3 q3)
{
    const float3x3 minv = float3x3(m.x, 0.0, m.y, 0.0, 1.0, 0.0, m.z, 0.0, m.w);
    float3 L[5];
    L[0] = mul(minv, q0);
    L[1] = mul(minv, q1);
    L[2] = mul(minv, q2);
    L[3] = mul(minv, q3);
    L[4] = L[0];
    const uint n = ltcClip(L);
    if (n == 0u) return 0.0;
    [unroll]
    for (uint k = 0u; k < 5u; ++k) L[k] = normalize(L[k]);
    float sum = ltcEdge(L[0], L[1]).z + ltcEdge(L[1], L[2]).z + ltcEdge(L[2], L[3]).z;
    if (n >= 4u) sum += ltcEdge(L[3], L[4]).z;
    if (n == 5u) sum += ltcEdge(L[4], L[0]).z;
    return abs(sum);   // ltcEdge's fit holds the 1 / (2 pi)
}

// ---- refraction (step 3) ---------------------------------------------------------------------
// GGX with two alphas a (an isotropic lobe passes (alpha, alpha): the same D and Lambda), its
// visible normals unbounded (refraction needs the normals bounded sampling leaves out), and
// Walter et al. 2007's rough refraction. eta is the relative IOR, wi's side over wo's.

float3 ggxSampleVisible(float3 wo, float2 alpha, float2 u)
{
    const float3 woStd = normalize(float3(wo.xy * alpha, wo.z));
    const float phi = (2.0 * u.x - 1.0) * kPbrPi;
    const float z = (1.0 - u.y) * (1.0 + woStd.z) - woStd.z;
    const float sinTheta = sqrt(saturate(1.0 - z * z));
    const float3 mStd = float3(sinTheta * cos(phi), sinTheta * sin(phi), z) + woStd;
    return normalize(float3(mStd.xy * alpha, mStd.z));
}

// The density of m under ggxSampleVisible: G1(wo) max(0, wo.m) D(m) / wo.z.
float ggxVisiblePdf(float3 wo, float3 m, float2 alpha)
{
    const float c = dot(wo, m);
    if (!(c > 0.0) || !(wo.z > 0.0)) return 0.0;
    return c * ggxDAniso(m, alpha) / ((1.0 + ggxLambdaAniso(wo, alpha)) * wo.z);
}

// wi refracted from wo through m (eta = n_wi / n_wo); false at total internal reflection.
bool refractThrough(float3 wo, float3 m, float eta, out float3 wi)
{
    const float c = dot(wo, m);
    const float s2 = (1.0 - c * c) / (eta * eta);
    wi = float3(0.0, 0.0, -1.0);
    if (s2 >= 1.0) return false;
    wi = normalize(-wo / eta + (c / eta - sqrt(1.0 - s2)) * m);
    return true;
}

// The refraction's half vector (toward wo's side), or 0 when wo and wi cannot be a refraction.
float3 refractHalf(float3 wo, float3 wi, float eta)
{
    float3 h = wo + eta * wi;
    const float l = length(h);
    if (!(l > 1e-6)) return float3(0.0);
    h /= l;
    if (h.z < 0.0) h = -h;
    if (!(dot(wo, h) > 0.0) || !(dot(wi, h) < 0.0)) return float3(0.0);
    return h;
}

// Single-scatter refraction x |cos(wi)| (wo.z > 0 > wi.z), with (1 - F): Walter et al. 2007, in
// the form whose integral over wi is the energy refracted (no eta^2 radiance scaling: it cancels
// through a closed surface, and keeps the albedo at most 1).
float ggxRefract(float3 wo, float3 wi, float2 alpha, float eta)
{
    const float3 h = refractHalf(wo, wi, eta);
    if (!(h.z > 0.0)) return 0.0;
    const float co = dot(wo, h), ci = dot(wi, h);
    const float denom = co + eta * ci;
    const float G2 = 1.0 / (1.0 + ggxLambdaAniso(wo, alpha) + ggxLambdaAniso(float3(wi.xy, -wi.z), alpha));
    const float F = fresnelDielectric(co, eta);
    return (1.0 - F) * ggxDAniso(h, alpha) * G2 * co * abs(ci) * eta * eta / (wo.z * denom * denom);
}

// The density of wi under refraction through ggxSampleVisible's normals.
float ggxRefractPdf(float3 wo, float3 wi, float2 alpha, float eta)
{
    const float3 h = refractHalf(wo, wi, eta);
    if (!(h.z > 0.0)) return 0.0;
    const float denom = dot(wo, h) + eta * dot(wi, h);
    return ggxVisiblePdf(wo, h, alpha) * eta * eta * abs(dot(wi, h)) / (denom * denom);
}

// Dispersion (OpenPBR): the IOR at wavelength lambda (nm) of a glass whose IOR at the d line is
// nd, its Abbe number abbe / scale: n = nd + (nd - 1) / V (523655 / lambda^2 - 1.5168).
float dispersedIor(float nd, float abbe, float scale, float lambda)
{
    if (!(scale > 0.0) || !(abbe > 0.0)) return nd;
    const float V = abbe / scale;
    return nd + (nd - 1.0) / V * (523655.0 / (lambda * lambda) - 1.5168);
}

// ---- the thin film (step 3) -------------------------------------------------------------------
// Belcour & Barla 2017, "A Practical Extension to Microfacet Theory for the Modeling of Varying
// Iridescence": the Airy reflectance of a film of IOR filmIor and thickness d (nm) between the
// outside (IOR 1) and a base of normal-incidence reflectance F0, in RGB through their Gaussian fits
// of the CIE colour matching functions (the paper's supplemental), two orders of the sum.

float3 airySensitivity(float opd, float3 shift)
{
    const float phase = 2.0 * kPbrPi * opd * 1e-9;
    const float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    const float3 pos = float3(1.6810e6, 1.7953e6, 2.2084e6);
    const float3 var = float3(4.3278e9, 9.3046e9, 6.6121e9);
    float3 xyz = val * sqrt(2.0 * kPbrPi * var) * cos(pos * phase + shift) * exp(-phase * phase * var);
    xyz.x += 9.7470e-14 * sqrt(2.0 * kPbrPi * 4.5282e9) * cos(2.2399e6 * phase + shift.x) * exp(-4.5282e9 * phase * phase);
    xyz /= 1.0685e-7;
    return float3(3.2404542 * xyz.x - 1.5371385 * xyz.y - 0.4985314 * xyz.z,
                  -0.9692660 * xyz.x + 1.8760108 * xyz.y + 0.0415560 * xyz.z,
                  0.0556434 * xyz.x - 0.2040259 * xyz.y + 1.0572252 * xyz.z);
}

float pbrIorToF0(float a, float b)
{
    const float r = (a - b) / (a + b);
    return r * r;
}

float3 airyReflectance(float cosTheta1, float filmIor, float thickness, float3 baseF0)
{
    // A film thinner than 30 nm fades to none (its IOR toward the outside's).
    const float n2 = lerp(1.0, filmIor, smoothstep(0.0, 30.0, thickness));
    const float sin2 = (1.0 - cosTheta1 * cosTheta1) / (n2 * n2);
    if (sin2 >= 1.0) return float3(1.0);
    const float cosTheta2 = sqrt(1.0 - sin2);
    const float R12 = fresnelDielectric(cosTheta1, n2);
    const float T121 = 1.0 - R12;
    const float phi12 = n2 < 1.0 ? kPbrPi : 0.0;
    const float phi21 = kPbrPi - phi12;
    const float3 f0 = clamp(baseF0, float3(0.0), float3(0.9999));
    const float3 sf0 = sqrt(f0);
    const float3 n3 = (1.0 + sf0) / (1.0 - sf0);   // the base's IOR from its F0
    const float3 R1 = float3(pbrIorToF0(n3.x, n2), pbrIorToF0(n3.y, n2), pbrIorToF0(n3.z, n2));
    const float m5 = pow(1.0 - cosTheta2, 5.0);
    const float3 R23 = R1 + (1.0 - R1) * m5;
    const float3 phi23 = float3(n3.x < n2 ? kPbrPi : 0.0, n3.y < n2 ? kPbrPi : 0.0, n3.z < n2 ? kPbrPi : 0.0);
    const float opd = 2.0 * n2 * thickness * cosTheta2;
    const float3 phi = phi21 + phi23;
    const float3 R123 = clamp(R12 * R23, float3(1e-5), float3(0.9999));
    const float3 r123 = sqrt(R123);
    const float3 Rs = T121 * T121 * R23 / (1.0 - R123);
    float3 I = R12 + Rs;
    float3 Cm = Rs - T121;
    [unroll]
    for (uint m = 1u; m <= 2u; ++m)
    {
        Cm *= r123;
        I += Cm * 2.0 * airySensitivity(float(m) * opd, float(m) * phi);
    }
    return max(I, float3(0.0));
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

// The dielectric's single scatter changed by a change d of the lobe's S5 and S6: its exact Fresnel
// in F82's form, F0 + (1 - F0)(1 - c)^5 - b c (1 - c)^6, b fitted to it at c = 1/7.
float dielectricS56(float eta, float2 d)
{
    if (all(d == float2(0.0))) return 0.0;
    const float r = (eta - 1.0) / (eta + 1.0);
    const float F0 = r * r;
    const float m = 1.0 - kF82Mu;
    const float m5 = m * m * m * m * m;
    const float b = (F0 + (1.0 - F0) * m5 - fresnelDielectric(kF82Mu, eta)) / (kF82Mu * m5 * m);
    return (1.0 - F0) * d.x - b * d.y;
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
    float transmissionWeight;   // transmission_weight
    float3 transmissionColor;   // transmission_color (at the surface; transmission_depth's volume is the path tracer's)
    bool thinWalled;            // geometry_thin_walled
    float thinFilmWeight;       // thin_film_weight
    float thinFilmThickness;    // thin_film_thickness (micrometres)
    float thinFilmIor;          // thin_film_ior
    float subsurfaceWeight;     // subsurface_weight
    float3 subsurfaceColor;     // subsurface_color
    bool subsurfaceLocal;       // true: the subsurface as a local diffuse lobe; false: lobe 5, walked (the path tracer)
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
    m.transmissionWeight = 0.0;
    m.transmissionColor = float3(1.0);
    m.thinWalled = false;
    m.thinFilmWeight = 0.0;
    m.thinFilmThickness = 0.5;
    m.thinFilmIor = 1.4;
    m.subsurfaceWeight = 0.0;
    m.subsurfaceColor = float3(0.8);
    m.subsurfaceLocal = true;
    return m;
}

// Van de Hulst's inversion (Chiang, Kutz & Burley 2016): the single-scatter albedo whose volume's
// multiple scattering shows albedo A (subsurface_color), per channel.
float3 subsurfaceSingleScatter(float3 A)
{
    const float3 a = saturate(A);
    const float3 k = 4.09712 + 4.20863 * a - sqrt(9.59217 + 41.6808 * a + 17.7126 * a * a);
    return saturate(1.0 - k * k);
}

float pbrLuminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// What sample() returns: the direction, f cos / pdf, the pdf (0: no sample) and the lobe it
// came from (0 the base's specular, 1 the diffuse, 2 the coat, 3 the fuzz, 4 the transmission, 5 the
// subsurface walked: wi points into the surface, weight is what enters, the volume decides the rest).
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
    float2 s56Delta;     // viewedFrom()'s pbrS56Delta: the anisotropic lobe's Fresnel moments
    float eta;           // the dielectric's IOR after specular_weight
    float3 specColor;
    float3 metalF0;      // specular_weight folded in below
    float3 metalB;
    float metalWeight;   // specular_weight on the metal
    float transmission;  // transmission_weight (the dielectric's share that refracts instead of the diffuse)
    float3 transTint;    // transmission_color, entering
    bool thinWalled;     // geometry_thin_walled: a slab, refraction straight through
    float film;          // thin_film_weight
    float filmThickness; // nm
    float filmIor;
    float sss;           // subsurface_weight: its share of the dielectric's diffuse slot
    float3 sssColor;     // subsurface_color
    bool sssLocal;       // the subsurface as a local diffuse lobe (else walked, lobe 5)

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
        s56Delta = float2(0.0);
        eta = clamp(modulatedIor(m.specularIor, m.specularWeight), 1.0, kPbrEtaMax);
        specColor = saturate(m.specularColor);
        metalF0 = rho;
        metalB = f82B(metalF0, specColor);
        metalWeight = saturate(m.specularWeight);
        transmission = saturate(m.transmissionWeight);
        transTint = saturate(m.transmissionColor);
        thinWalled = m.thinWalled;
        film = saturate(m.thinFilmWeight);
        filmThickness = max(m.thinFilmThickness, 0.0) * 1000.0;
        filmIor = max(m.thinFilmIor, 1.0);
        sss = saturate(m.subsurfaceWeight);
        sssColor = saturate(m.subsurfaceColor);
        sssLocal = m.subsurfaceLocal;
    }

    // ---- subsurface (step 4) ----
    // The dielectric's diffuse slot under the specular is lerp(diffuse, subsurface, S). Local: the
    // subsurface is a diffuse lobe of albedo subsurface_color (EON at the base's diffuse roughness),
    // which the deferred frame then spreads; walked (the path tracer): it is lobe 5.
    bool walked() { return sss > 0.0 && metalness < 1.0 && !sssLocal; }

    float3 slotEval(float3 wi, float3 wo)
    {
        if (!(sss > 0.0)) return eonEval(rho, diffRough, wi, wo);
        const float3 d = (1.0 - sss) * eonEval(rho, diffRough, wi, wo);
        return sssLocal ? d + sss * eonEval(sssColor, diffRough, wi, wo) : d;
    }

    float3 slotAlbedo(float mu)
    {
        if (!(sss > 0.0)) return eonAlbedo(rho, diffRough, mu);
        const float3 d = (1.0 - sss) * eonAlbedo(rho, diffRough, mu);
        return sssLocal ? d + sss * eonAlbedo(sssColor, diffRough, mu) : d;
    }

    // What enters the volume (walked): (1 - metalness)(1 - T) S (1 - the specular's albedo).
    float subsurfaceShare(float3 wo, OpenPBRTables t)
    {
        if (!walked() || wo.z <= 0.0) return 0.0;
        return (1.0 - metalness) * (1.0 - transmission) * sss * (1.0 - dielectricAlbedo(wo.z, t));
    }

    // Its expected return (the walk's colour is subsurface_color's), for choosing lobes.
    float3 subsurfaceAlbedo(float3 wo, OpenPBRTables t) { return subsurfaceShare(wo, t) * sssColor; }

    // The base samples its own lobes (glass, a walked subsurface): OpenPBRSurface delegates to it.
    bool delegated() { return transmissive() || walked(); }

    // Light passes through this surface (some of its dielectric refracts).
    bool transmissive() { return transmission > 0.0 && metalness < 1.0; }

    // The dielectric's Fresnel at c with the thin film lerped in.
    float3 dielectricFresnel(float c)
    {
        const float F = fresnelDielectric(c, eta);
        if (!(film > 0.0)) return float3(F);
        return lerp(float3(F), airyReflectance(c, filmIor, filmThickness, float3(pbrIorToF0(eta, 1.0))), film);
    }

    float3 metalFresnel(float c)
    {
        const float3 F = fresnelF82(c, metalF0, metalB);
        if (!(film > 0.0)) return F;
        return lerp(F, airyReflectance(c, filmIor, filmThickness, metalF0), film);
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
        return t.EssD(mu, tableRough, eta) + dielectricS56(eta, s56Delta) + ms * (1.0 - t.E1(mu, tableRough));
    }

    float3 metalAlbedo(float mu, OpenPBRTables t)
    {
        const float e1 = t.E1(mu, tableRough);
        const float s5 = t.S5(mu, tableRough) + s56Delta.x, s6 = t.S6(mu, tableRough) + s56Delta.y;
        const float3 ess = metalWeight * (metalF0 * e1 + (1.0 - metalF0) * s5 - metalB * s6);
        return ess + msFresnel(metalFavg(), pbrE1Avg(t, rough, anisotropy)) * (1.0 - e1);
    }

    // The specular lobes (the metal's and the dielectric's, tinted) and the diffuse beneath them,
    // each x cos(wi): eval() is their sum. The deferred frame lights a sphere or a rectangle from a
    // different direction for each (core/deferred_lighting.slang).
    void evalLobes(float3 wo, float3 wi, OpenPBRTables t, out float3 specular, out float3 diffuse)
    {
        specular = float3(0.0);
        diffuse = float3(0.0);
        if (wo.z <= 0.0 || wi.z <= 0.0)
        {
            if (transmissive()) specular = evalTransmission(wo, wi, t);
            return;
        }
        const float3 m = normalize(wo + wi);
        const float c = dot(wo, m);
        const float ss = anisotropy > 0.0 ? ggxSingleAniso(wo, wi, alphaXY) : ggxSingle(wo, wi, alpha * alpha);
        const float ms = msLobe(wo, wi, t);
        const float e1Avg = pbrE1Avg(t, rough, anisotropy);
        if (metalness > 0.0)
        {
            const float3 F = metalWeight * metalFresnel(c);
            specular += metalness * (F * ss + msFresnel(metalFavg(), e1Avg) * ms);
        }
        if (metalness < 1.0)
        {
            if (film > 0.0)
                specular += (1.0 - metalness) * specColor
                          * (dielectricFresnel(c) * ss + msFresnel(float3(t.FavgD(eta)), e1Avg).x * ms);
            else
            {
                const float spec = fresnelDielectric(c, eta) * ss + msFresnel(float3(t.FavgD(eta)), e1Avg).x * ms;
                specular += (1.0 - metalness) * specColor * spec;
            }
            diffuse = (1.0 - metalness) * (1.0 - transmission) * (1.0 - dielectricAlbedo(wo.z, t))
                    * slotEval(wi, wo) * wi.z;
        }
    }

    // ---- transmission (step 3) ----
    // Seen from outside (wo.z > 0): the dielectric's (1 - metalness) x transmission share refracts
    // what its reflection leaves, 1 - dielectricAlbedo(wo), spread over the refraction lobe by its
    // single-scatter albedo (so a white glass passes all of it: Turquin 2019's scaling), tinted by
    // transmission_color. From inside (wo.z < 0, solid): the interface at 1 / eta, its reflection
    // single scatter (EssDIn) and its refraction everything else. Thin-walled: either side is the
    // outside, and the refraction is the reflection lobe mirrored through the surface.

    float3 flipZ(float3 v) { return float3(v.xy, -v.z); }

    // E1 at the view's table roughness: the thin slab's lobe's albedo.
    float thinE1(float3 wo, OpenPBRTables t) { return t.E1(wo.z, tableRough); }

    float refractedShare(float3 wo, OpenPBRTables t)
    {
        return (1.0 - metalness) * transmission * (1.0 - dielectricAlbedo(wo.z, t));
    }

    float3 evalTransmission(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (wo.z == 0.0 || wi.z == 0.0) return float3(0.0);
        if (thinWalled)
        {
            const float3 o = wo.z > 0.0 ? wo : flipZ(wo);
            const float3 i = wo.z > 0.0 ? wi : flipZ(wi);
            if (i.z >= 0.0) return float3(0.0);   // the reflection is evalLobes' (after the flip)
            const OpenPBRBase b = viewedFrom(o, t);
            const float3 r = flipZ(i);
            const float shape = anisotropy > 0.0 ? ggxSingleAniso(o, r, alphaXY) : ggxSingle(o, r, alpha * alpha);
            return transTint * (b.refractedShare(o, t) / max(b.thinE1(o, t), 1e-4)) * shape;   // ggxSingle is f x cos already
        }
        if (wo.z > 0.0)
        {
            if (wi.z >= 0.0) return float3(0.0);
            const OpenPBRBase b = viewedFrom(wo, t);
            const float et = t.EssT(wo.z, b.tableRough, eta);
            return transTint * (b.refractedShare(wo, t) / max(et, 1e-4)) * ggxRefract(wo, wi, alphaXY, eta);
        }
        // Inside a solid: the interface seen from below, at 1 / eta.
        // Both lobes over their single-scatter sum: total internal reflection moves energy between
        // them sharply with the view and the IOR, but not their sum, which interpolates cleanly
        // (filling 1 - EssDIn with the refraction left rough glass 2% dark seen from inside).
        const float3 o = flipZ(wo), i = flipZ(wi);
        const float n = 1.0 / eta;
        const float norm = 1.0 / max(insideSum(wo, t), 1e-4);
        if (i.z > 0.0)
        {
            const float3 m = normalize(o + i);
            const float ss = anisotropy > 0.0 ? ggxSingleAniso(o, i, alphaXY) : ggxSingle(o, i, alpha * alpha);
            return float3(fresnelDielectric(dot(o, m), n) * ss * norm);
        }
        return float3(ggxRefract(o, i, alphaXY, n) * norm);
    }

    // Inside a solid: the single-scatter reflection and refraction toward wo (EssDIn + EssTIn).
    float insideSum(float3 wo, OpenPBRTables t)
    {
        const float3 o = flipZ(wo);
        const float r = viewedFrom(o, t).tableRough;
        return t.EssDIn(o.z, r, eta) + t.EssTIn(o.z, r, eta);
    }

    // The refraction's albedo toward wo (outside): what passes, tinted.
    float3 transmissionAlbedo(float3 wo, OpenPBRTables t)
    {
        if (!transmissive() || wo.z <= 0.0) return float3(0.0);
        return transTint * refractedShare(wo, t);
    }

    // From inside a solid, reflection's share of what leaves (the chance of sampling it).
    float insideReflection(float3 wo, OpenPBRTables t)
    {
        const float3 o = flipZ(wo);
        const float r = viewedFrom(o, t).tableRough;
        const float er = t.EssDIn(o.z, r, eta), et = t.EssTIn(o.z, r, eta);
        return er + et > 0.0 ? saturate(er / (er + et)) : 1.0;
    }

    float pdfTransmissive(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (wo.z == 0.0 || wi.z == 0.0 || (wo.z < 0.0 && !transmissive())) return 0.0;
        if (!thinWalled && wo.z < 0.0)
        {
            const float3 o = flipZ(wo), i = flipZ(wi);
            const float pr = insideReflection(wo, t);
            return i.z > 0.0 ? pr * ggxPdfBoundedAniso(o, i, alphaXY) : (1.0 - pr) * ggxRefractPdf(o, i, alphaXY, 1.0 / eta);
        }
        const float3 o = wo.z > 0.0 ? wo : flipZ(wo);
        const float3 i = wo.z > 0.0 ? wi : flipZ(wi);
        const float4 s = transmissiveShares(o, t);
        if (i.z > 0.0) return s.x * pdfSpecular(o, i) + s.y * eonPdf(o, i, diffRough);
        return s.z * (thinWalled ? pdfSpecular(o, flipZ(i)) : ggxRefractPdf(o, i, alphaXY, eta));
    }

    // The chances of the reflection, the diffuse, the refraction and the walked subsurface from
    // outside, by luminance. The walk's share is no density over directions: pdf() leaves it out.
    float4 transmissiveShares(float3 wo, OpenPBRTables t)
    {
        const OpenPBRBase b = viewedFrom(wo, t);
        const float r = pbrLuminance(b.specularAlbedo(wo, t));
        const float d = pbrLuminance(b.diffuseAlbedo(wo, t));
        const float tr = pbrLuminance(b.transmissionAlbedo(wo, t));
        const float ss = pbrLuminance(b.subsurfaceAlbedo(wo, t));
        const float total = r + d + tr + ss;
        return total > 0.0 ? float4(r, d, tr, ss) / total : float4(1.0, 0.0, 0.0, 0.0);
    }

    BsdfSample sampleTransmissive(float3 wo, float3 u, OpenPBRTables t)
    {
        BsdfSample s;
        s.wi = float3(0.0, 0.0, 1.0);
        s.weight = float3(0.0);
        s.pdf = 0.0;
        s.lobe = 0u;
        if (wo.z == 0.0 || (wo.z < 0.0 && !transmissive())) return s;
        const bool inside = !thinWalled && wo.z < 0.0;
        const bool flip = wo.z < 0.0;
        const float3 o = flip ? flipZ(wo) : wo;
        float3 i;
        if (inside)
        {
            const float pr = insideReflection(wo, t);
            if (u.x < pr) i = reflect(-o, ggxSampleBoundedAniso(o, alphaXY, u.yz));
            else
            {
                if (!refractThrough(o, ggxSampleVisible(o, alphaXY, u.yz), 1.0 / eta, i)) return s;
                s.lobe = 4u;
            }
        }
        else
        {
            const float4 p = transmissiveShares(o, t);
            if (u.x < p.x) i = reflect(-o, sampleSpecular(o, u.yz));
            else if (u.x < p.x + p.y)
            {
                i = eonSample(o, diffRough, u.y, u.z).xyz;
                s.lobe = 1u;
            }
            else if (u.x < p.x + p.y + p.w)
            {
                // The subsurface: into the volume, cosine-weighted; what enters is the weight.
                const float phi = 2.0 * kPbrPi * u.z;
                const float r = sqrt(u.y);
                s.wi = float3(r * cos(phi), r * sin(phi), -sqrt(max(1.0 - u.y, 0.0)));
                s.lobe = 5u;
                s.pdf = p.w;
                s.weight = float3(subsurfaceShare(o, t) / p.w);
                return s;
            }
            else
            {
                if (thinWalled) i = flipZ(reflect(-o, sampleSpecular(o, u.yz)));
                else if (!refractThrough(o, ggxSampleVisible(o, alphaXY, u.yz), eta, i)) return s;
                s.lobe = 4u;
            }
        }
        if (s.lobe == 4u ? i.z >= 0.0 : i.z <= 0.0) return s;
        s.wi = flip ? flipZ(i) : i;
        s.pdf = pdfTransmissive(wo, s.wi, t);
        if (s.pdf > 0.0) s.weight = eval(wo, s.wi, t) / s.pdf;
        return s;
    }

    // BSDF x cos(wi).
    float3 eval(float3 wo, float3 wi, OpenPBRTables t)
    {
        float3 specular, diffuse;
        if (thinWalled && transmissive() && wo.z < 0.0)
            evalLobes(flipZ(wo), flipZ(wi), t, specular, diffuse);
        else
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
        b.s56Delta = float2(0.0);
        return b;
    }

    // This surface's albedos as seen from wo: an anisotropic lobe's are read from the isotropic
    // tables at the roughness whose E1 is its own there (OpenPBRTables.AnisoRough). Isotropic: this
    // surface unchanged.
    OpenPBRBase viewedFrom(float3 wo, OpenPBRTables t)
    {
        OpenPBRBase b = this;
        const float3 w = float3(wo.xy, abs(wo.z));
        b.tableRough = pbrTableRough(t, rough, anisotropy, w);
        b.s56Delta = pbrS56Delta(t, rough, anisotropy, w, b.tableRough);
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
        return (1.0 - metalness) * (1.0 - transmission) * (1.0 - dielectricAlbedo(wo.z, t)) * slotAlbedo(wo.z);
    }

    float3 albedo(float3 wo, OpenPBRTables t)
    {
        return specularAlbedo(wo, t) + diffuseAlbedo(wo, t) + transmissionAlbedo(wo, t) + subsurfaceAlbedo(wo, t);
    }

    // The chance of sampling the specular lobe (GGX, the metal and the dielectric's together):
    // its share of the albedo's luminance.
    float specularShare(float3 wo, OpenPBRTables t)
    {
        const float ed = dielectricAlbedo(wo.z, t);
        const float spec = metalness * pbrLuminance(metalAlbedo(wo.z, t)) + (1.0 - metalness) * pbrLuminance(specColor) * ed;
        const float diff = (1.0 - metalness) * (1.0 - transmission) * (1.0 - ed) * pbrLuminance(slotAlbedo(wo.z));
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
        if (delegated()) return pdfTransmissive(wo, wi, t);
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
        if (delegated()) return sampleTransmissive(wo, u, t);
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
        return t.EssD(wo.z, r, eta) + dielectricS56(eta, pbrS56Delta(t, rough, anisotropy, wo, r))
             + ms * (1.0 - t.E1(wo.z, r));
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
        if (layered && wo.z > 0.0 && wi.z < 0.0)   // refracted: through the coat and the fuzz, as the diffuse goes
        {
            specular *= baseScale(coatPathDiffuse(), wo, t);
            return;
        }
        if (!layered || wo.z <= 0.0 || wi.z <= 0.0) return;
        const float3 k = baseScale(coatPath(wi.z), wo, t);
        const float fuzzOver = 1.0 - fuzzWeight * fuzz.albedo(wo.z);
        specular = k * specular + fuzzOver * coatWeight * coat.eval(wo, wi, t);
        diffuse = k * diffuse + fuzzWeight * fuzzColor * fuzz.albedo(wo.z) * fuzz.D(wo, wi);
    }

    // A thin wall is the same from either side (OpenPBR: either side is the outside): seen from its back it is
    // evaluated, drawn and weighed as its front, mirrored through the surface. The camera side always meets a thin
    // wall from its front (its frame faces the ray); the light pass meets it from the light's side.
    bool thinBack(float3 wo) { return base.thinWalled && base.transmissive() && wo.z < 0.0; }
    float3 mirrored(float3 v) { return float3(v.xy, -v.z); }

    float3 eval(float3 wo, float3 wi, OpenPBRTables t)
    {
        return thinBack(wo) ? evalFront(mirrored(wo), mirrored(wi), t) : evalFront(wo, wi, t);
    }

    float3 evalFront(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (base.transmissive() && wo.z < 0.0) return base.eval(wo, wi, t);
        float3 specular, diffuse;
        evalLobes(wo, wi, t, specular, diffuse);
        return specular + diffuse;
    }

    // ---- the smooth and rough parts (sub-project 24, caustics) ----
    // eval() split by lobe roughness: the smooth part holds the base's specular reflection and
    // refraction while the base's roughness is under cut, and the coat's lobe while the coat's is;
    // the rough part holds everything else eval() holds (the diffuse, the fuzz, and either specular
    // at or over the cut). smooth + rough = eval() in every case (the same arithmetic, not a call of
    // it, so eval()'s own code and bits stay as they are). Light traced from a light follows only
    // smooth parts and reaches the camera only through rough ones; the camera side drops exactly
    // those terms (docs/specs/2026-11-03-caustics-design.md, "The split").
    void evalParts(float3 wo, float3 wi, OpenPBRTables t, float cut, out float3 smooth, out float3 rough)
    {
        if (thinBack(wo)) evalPartsFront(mirrored(wo), mirrored(wi), t, cut, smooth, rough);
        else evalPartsFront(wo, wi, t, cut, smooth, rough);
    }

    void evalPartsFront(float3 wo, float3 wi, OpenPBRTables t, float cut, out float3 smooth, out float3 rough)
    {
        float3 s, d;
        float3 c = float3(0.0);
        if (base.transmissive() && wo.z < 0.0)
        {
            // Inside a solid, or a thin wall from its back: the base alone, as eval() takes it.
            if (base.thinWalled) base.evalLobes(base.flipZ(wo), base.flipZ(wi), t, s, d);
            else base.evalLobes(wo, wi, t, s, d);
        }
        else
        {
            baseAt(wo, t).evalLobes(wo, wi, t, s, d);
            if (layered && wo.z > 0.0 && wi.z < 0.0)
            {
                s *= baseScale(coatPathDiffuse(), wo, t);
            }
            else if (layered && wo.z > 0.0 && wi.z > 0.0)
            {
                const float3 k = baseScale(coatPath(wi.z), wo, t);
                const float fuzzOver = 1.0 - fuzzWeight * fuzz.albedo(wo.z);
                s = k * s;
                c = float3(fuzzOver * coatWeight * coat.eval(wo, wi, t));
                d = k * d + fuzzWeight * fuzzColor * fuzz.albedo(wo.z) * fuzz.D(wo, wi);
            }
        }
        const bool bs = base.rough < cut;
        const bool cs = coatWeight > 0.0 && coat.rough < cut;
        smooth = (bs ? s : float3(0.0)) + (cs ? c : float3(0.0));
        rough = d + (bs ? float3(0.0) : s) + (cs ? float3(0.0) : c);
    }

    // Whether any lobe can be smooth at cut.
    bool hasSmooth(float cut) { return base.rough < cut || (coatWeight > 0.0 && coat.rough < cut); }

    // The chances of the smooth lobes from wo, by the luminance of their albedos: (the base's
    // reflection, the base's refraction, the coat). 0 when nothing is smooth.
    float3 smoothShares(float3 wo, OpenPBRTables t, float cut)
    {
        const bool bs = base.rough < cut;
        const bool cs = coatWeight > 0.0 && coat.rough < cut;
        float3 w = float3(0.0);
        if (base.transmissive() && wo.z < 0.0)
        {
            if (!bs) return w;
            if (!base.thinWalled)
            {
                const float pr = base.insideReflection(wo, t);
                return float3(pr, 1.0 - pr, 0.0);
            }
            const float4 p = base.transmissiveShares(base.flipZ(wo), t);
            w = float3(p.x, p.z, 0.0);
        }
        else
        {
            if (wo.z <= 0.0) return w;
            const OpenPBRBase b = baseAt(wo, t);
            if (bs)
            {
                w.x = pbrLuminance(baseScale(coatPath(wo.z), wo, t) * b.specularAlbedo(wo, t));
                w.y = pbrLuminance(baseScale(coatPathDiffuse(), wo, t) * b.transmissionAlbedo(wo, t));
            }
            if (cs) w.z = coatAlbedo(wo, t);
        }
        const float total = w.x + w.y + w.z;
        return total > 0.0 ? w / total : float3(0.0);
    }

    // The density sampleSmooth() draws wi with.
    float pdfSmooth(float3 wo, float3 wi, OpenPBRTables t, float cut)
    {
        if (wo.z == 0.0 || wi.z == 0.0) return 0.0;
        const float3 p = smoothShares(wo, t, cut);
        if (base.transmissive() && wo.z < 0.0)
        {
            const float3 o = base.flipZ(wo), i = base.flipZ(wi);
            if (base.thinWalled)
                return i.z > 0.0 ? p.x * base.pdfSpecular(o, i) : p.y * base.pdfSpecular(o, base.flipZ(i));
            return i.z > 0.0 ? p.x * ggxPdfBoundedAniso(o, i, base.alphaXY)
                             : p.y * ggxRefractPdf(o, i, base.alphaXY, 1.0 / base.eta);
        }
        if (wo.z < 0.0) return 0.0;
        if (wi.z > 0.0) return p.x * base.pdfSpecular(wo, wi) + p.z * coat.pdf(wo, wi);
        return p.y * (base.thinWalled ? base.pdfSpecular(wo, base.flipZ(wi)) : ggxRefractPdf(wo, wi, base.alphaXY, base.eta));
    }

    // A direction from the smooth lobes alone, weighed by the smooth part over pdfSmooth() (a lobe
    // drawn by smoothShares; lobe 0 the base's reflection, 4 its refraction, 2 the coat).
    // sampleSmooth's direction and density alone (weight left 0): the light pass weighs it itself, in the
    // camera side's roles, so the smooth part is not evaluated twice.
    BsdfSample sampleSmoothDirection(float3 wo, float3 u, OpenPBRTables t, float cut)
    {
        BsdfSample s;
        s.wi = float3(0.0, 0.0, 1.0);
        s.weight = float3(0.0);
        s.pdf = 0.0;
        s.lobe = 0u;
        const float3 p = smoothShares(wo, t, cut);
        if (!(p.x + p.y + p.z > 0.0)) return s;
        const bool inside = base.transmissive() && wo.z < 0.0;
        const float3 o = inside ? base.flipZ(wo) : wo;
        float3 i;
        if (u.x < p.x)
        {
            i = inside && !base.thinWalled ? reflect(-o, ggxSampleBoundedAniso(o, base.alphaXY, u.yz))
                                           : reflect(-o, base.sampleSpecular(o, u.yz));
            if (i.z <= 0.0) return s;
        }
        else if (u.x < p.x + p.y)
        {
            if (base.thinWalled) i = base.flipZ(reflect(-o, base.sampleSpecular(o, u.yz)));
            else if (!refractThrough(o, ggxSampleVisible(o, base.alphaXY, u.yz), inside ? 1.0 / base.eta : base.eta, i)) return s;
            s.lobe = 4u;
            if (i.z >= 0.0) return s;
        }
        else
        {
            i = reflect(-o, coat.sampleNormal(o, u.yz));
            s.lobe = 2u;
            if (i.z <= 0.0) return s;
        }
        s.wi = inside ? base.flipZ(i) : i;
        s.pdf = pdfSmooth(wo, s.wi, t, cut);
        return s;
    }

    BsdfSample sampleSmooth(float3 wo, float3 u, OpenPBRTables t, float cut)
    {
        BsdfSample s = sampleSmoothDirection(wo, u, t, cut);
        if (s.pdf > 0.0)
        {
            float3 sm, ro;
            evalParts(wo, s.wi, t, cut, sm, ro);
            s.weight = sm / s.pdf;
        }
        return s;
    }

    // evalLobes (the specular and diffuse split light sampling uses) with the smooth part of the same terms:
    // one evaluation where light sampling needs both (raygenCaustics).
    void evalLobesSmooth(float3 wo, float3 wi, OpenPBRTables t, float cut, out float3 specular, out float3 diffuse,
                         out float3 smooth)
    {
        if (thinBack(wo)) evalLobesSmoothFront(mirrored(wo), mirrored(wi), t, cut, specular, diffuse, smooth);
        else evalLobesSmoothFront(wo, wi, t, cut, specular, diffuse, smooth);
    }

    void evalLobesSmoothFront(float3 wo, float3 wi, OpenPBRTables t, float cut, out float3 specular, out float3 diffuse,
                              out float3 smooth)
    {
        float3 s, d;
        float3 c = float3(0.0);
        baseAt(wo, t).evalLobes(wo, wi, t, s, d);
        if (layered && wo.z > 0.0 && wi.z < 0.0)
        {
            s *= baseScale(coatPathDiffuse(), wo, t);
        }
        else if (layered && wo.z > 0.0 && wi.z > 0.0)
        {
            const float3 k = baseScale(coatPath(wi.z), wo, t);
            const float fuzzOver = 1.0 - fuzzWeight * fuzz.albedo(wo.z);
            s = k * s;
            c = float3(fuzzOver * coatWeight * coat.eval(wo, wi, t));
            d = k * d + fuzzWeight * fuzzColor * fuzz.albedo(wo.z) * fuzz.D(wo, wi);
        }
        specular = s + c;
        diffuse = d;
        smooth = (base.rough < cut ? s : float3(0.0)) + (coatWeight > 0.0 && coat.rough < cut ? c : float3(0.0));
    }

    // ---- the smooth part's core (sub-project 24 step 2, docs/specs/2026-11-04-caustics-step2-design.md) ----
    // Whether the half vector m (the frame's z up) lies in a lobe's core: its slope stretched by the lobe's
    // alphas, (m.x / ax)^2 + (m.y / ay)^2 over m.z^2, at most core.x (GGX holds core.x / (1 + core.x) of its
    // normals there), or its plain slope's square at most core.y (a mirror's tail past a few alphas is still far
    // too narrow for the camera side to find: the core reaches a fixed angle at least). The manifold pass owns a
    // smooth reflection's core; the camera side keeps its tail.
    static bool inCore(float3 m, float2 a, float2 core)
    {
        if (m.z <= 0.0) return false;
        const float x = m.x / a.x, y = m.y / a.y, zz = m.z * m.z;
        return x * x + y * y <= core.x * zz || m.x * m.x + m.y * m.y <= core.y * zz;
    }

    // evalParts' smooth part for a reflection (wo and wi above; 0 otherwise), each smooth lobe kept only where
    // the half vector is in its own core (the base's alphaXY, the coat's).
    float3 evalSmoothCore(float3 wo, float3 wi, OpenPBRTables t, float cut, float2 coreSlope)
    {
        float3 specular, diffuse, smooth, core;
        evalLobesSmoothCore(wo, wi, t, cut, coreSlope, specular, diffuse, smooth, core);
        return core;
    }

    // evalLobesSmooth and the core of its smooth part, from one evaluation (raygenCaustics' light sample).
    void evalLobesSmoothCore(float3 wo, float3 wi, OpenPBRTables t, float cut, float2 coreSlope, out float3 specular,
                             out float3 diffuse, out float3 smooth, out float3 core)
    {
        const bool back = thinBack(wo);
        const float3 o = back ? mirrored(wo) : wo, i = back ? mirrored(wi) : wi;
        float3 s, d;
        float3 c = float3(0.0);
        baseAt(o, t).evalLobes(o, i, t, s, d);
        if (layered && o.z > 0.0 && i.z < 0.0)
        {
            s *= baseScale(coatPathDiffuse(), o, t);
        }
        else if (layered && o.z > 0.0 && i.z > 0.0)
        {
            const float3 k = baseScale(coatPath(i.z), o, t);
            const float fuzzOver = 1.0 - fuzzWeight * fuzz.albedo(o.z);
            s = k * s;
            c = float3(fuzzOver * coatWeight * coat.eval(o, i, t));
            d = k * d + fuzzWeight * fuzzColor * fuzz.albedo(o.z) * fuzz.D(o, i);
        }
        specular = s + c;
        diffuse = d;
        const bool bs = base.rough < cut, cs = coatWeight > 0.0 && coat.rough < cut;
        smooth = (bs ? s : float3(0.0)) + (cs ? c : float3(0.0));
        core = float3(0.0);
        if (back || !(o.z > 0.0) || !(i.z > 0.0)) return;
        const float3 m = normalize(o + i);
        if (bs && inCore(m, base.alphaXY, coreSlope)) core += s;
        if (cs && inCore(m, coat.alphaXY, coreSlope)) core += c;
    }

    // Per lobe (the base's specular, the coat), the largest alpha it can have where it is smooth: the larger
    // of its alphaXY at min(roughness, cut) (0 for a coat of no weight). Built from a material's factors it
    // bounds every point of the material (a roughness texture only lowers the roughness, and a coat's
    // widening of the base grows with it); whether a lobe can be smooth at all (a texture may take a rough
    // factor under the cut) is the host's to say.
    float2 smoothAlphaBounds(float cut)
    {
        const float2 b = ggxAnisoAlpha(min(base.rough, cut), base.anisotropy);
        const float2 c = ggxAnisoAlpha(min(coat.rough, cut), coat.anisotropy);
        return float2(max(b.x, b.y), coatWeight > 0.0 ? max(c.x, c.y) : 0.0);
    }

    // The refraction's albedo toward wo, through the layers.
    float3 transmissionAlbedo(float3 wo, OpenPBRTables t)
    {
        const float3 a = baseAt(wo, t).transmissionAlbedo(wo, t);
        if (!layered || wo.z <= 0.0) return a;
        return baseScale(coatPathDiffuse(), wo, t) * a;
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
        return specularAlbedo(wo, t) + diffuseAlbedo(wo, t) + transmissionAlbedo(wo, t) + subsurfaceAlbedo(wo, t);
    }

    // A walked subsurface's expected return toward wo, through the layers.
    float3 subsurfaceAlbedo(float3 wo, OpenPBRTables t)
    {
        const float3 a = baseAt(wo, t).subsurfaceAlbedo(wo, t);
        if (!layered || wo.z <= 0.0) return a;
        return baseScale(coatPathDiffuse(), wo, t) * a;
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
        return thinBack(wo) ? pdfFront(mirrored(wo), mirrored(wi), t) : pdfFront(wo, wi, t);
    }

    float pdfFront(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (!layered || (base.transmissive() && wo.z < 0.0)) return baseAt(wo, t).pdf(wo, wi, t);
        if (base.delegated()) return pdfLayeredTransmissive(wo, wi, t);
        if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;
        return pdfOf(wo, wi, lobeShares(wo, t));
    }

    // A layered glass: the base (all its lobes, by its own sampling), the coat and the fuzz, by the
    // luminance of their albedos.
    float3 layeredTransmissiveShares(float3 wo, OpenPBRTables t)
    {
        const float b = pbrLuminance(specularAlbedo(wo, t) - coatAlbedo(wo, t) + diffuseAlbedo(wo, t)
                                     - fuzzWeight * fuzzColor * fuzz.albedo(wo.z) + transmissionAlbedo(wo, t)
                                     + subsurfaceAlbedo(wo, t));
        const float c = coatAlbedo(wo, t);
        const float f = fuzzWeight * pbrLuminance(fuzzColor) * fuzz.albedo(wo.z);
        const float total = max(b, 0.0) + c + f;
        return total > 0.0 ? float3(max(b, 0.0), c, f) / total : float3(1.0, 0.0, 0.0);
    }

    float pdfLayeredTransmissive(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (wo.z <= 0.0) return 0.0;
        const float3 p = layeredTransmissiveShares(wo, t);
        float pdf = p.x * baseAt(wo, t).pdf(wo, wi, t);
        if (wi.z > 0.0) pdf += p.y * coat.pdf(wo, wi) + p.z * fuzz.D(wo, wi);
        return pdf;
    }

    BsdfSample sample(float3 wo, float3 u, OpenPBRTables t)
    {
        if (!thinBack(wo)) return sampleFront(wo, u, t);
        BsdfSample s = sampleFront(mirrored(wo), u, t);
        s.wi = mirrored(s.wi);
        return s;
    }

    BsdfSample sampleFront(float3 wo, float3 u, OpenPBRTables t)
    {
        if (!layered || (base.transmissive() && wo.z < 0.0)) return baseAt(wo, t).sample(wo, u, t);
        BsdfSample s;
        s.wi = float3(0.0, 0.0, 1.0);
        s.weight = float3(0.0);
        s.pdf = 0.0;
        s.lobe = 0u;
        if (wo.z <= 0.0) return s;
        if (base.delegated())
        {
            const float3 p = layeredTransmissiveShares(wo, t);
            if (u.x < p.x)
            {
                const BsdfSample b = baseAt(wo, t).sample(wo, float3(u.x / max(p.x, 1e-6), u.y, u.z), t);
                s.wi = b.wi;
                s.lobe = b.lobe;
                if (!(b.pdf > 0.0)) return s;
                if (b.lobe == 5u)   // into the volume, through the coat and the fuzz as the diffuse goes
                {
                    s.pdf = b.pdf * p.x;
                    s.weight = baseScale(coatPathDiffuse(), wo, t) * b.weight / max(p.x, 1e-6);
                    return s;
                }
            }
            else if (u.x < p.x + p.y)
            {
                s.wi = reflect(-wo, coat.sampleNormal(wo, u.yz));
                s.lobe = 2u;
            }
            else
            {
                s.wi = fuzz.sample(wo, u.yz);
                s.lobe = 3u;
            }
            if (s.wi.z == 0.0 || ((s.lobe == 2u || s.lobe == 3u) && s.wi.z < 0.0)) return s;   // the coat and fuzz reflect only
            s.pdf = pdfLayeredTransmissive(wo, s.wi, t);
            if (s.pdf > 0.0) s.weight = eval(wo, s.wi, t) / s.pdf;
            return s;
        }
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
