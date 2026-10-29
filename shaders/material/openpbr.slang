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
static const uint kPbrTableFloats = kPbrOffEssD + kPbrDielMu * kPbrDielRough * kPbrDielEta;

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
    // The dielectric's single-scatter albedo (exact Fresnel), trilinear in (mu, rough, eta).
    float EssD(float mu, float rough, float eta)
    {
        uint im, ir, ie;
        float fm, fr, fe;
        pbrGridCoord(sqrt(saturate(mu)), kPbrDielMu, im, fm);
        pbrGridCoord(rough, kPbrDielRough, ir, fr);
        pbrGridCoord((eta - 1.0) / (kPbrEtaMax - 1.0), kPbrDielEta, ie, fe);
        float v[2];
        [unroll]
        for (uint k = 0u; k < 2u; ++k)
        {
            const uint e0 = kPbrOffEssD + (ie + k) * kPbrDielRough * kPbrDielMu;
            const uint r0 = e0 + ir * kPbrDielMu, r1 = r0 + kPbrDielMu;
            v[k] = lerp(lerp(data[r0 + im], data[r0 + im + 1u], fm), lerp(data[r1 + im], data[r1 + im + 1u], fm), fr);
        }
        return lerp(v[0], v[1], fe);
    }
};

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
    return m;
}

float pbrLuminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// What sample() returns: the direction, f cos / pdf, the pdf (0: no sample) and the lobe it
// came from (0 specular, 1 diffuse).
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
        const float e1Avg = t.E1Avg(rough);
        return (1.0 - t.E1(wo.z, rough)) * (1.0 - t.E1(wi.z, rough)) / (kPbrPi * max(1.0 - e1Avg, 1e-6)) * wi.z;
    }

    // The dielectric specular's albedo toward wo (untinted: the albedo scaling's E).
    float dielectricAlbedo(float mu, OpenPBRTables t)
    {
        const float ms = msFresnel(float3(t.FavgD(eta)), t.E1Avg(rough)).x;
        return t.EssD(mu, rough, eta) + ms * (1.0 - t.E1(mu, rough));
    }

    float3 metalAlbedo(float mu, OpenPBRTables t)
    {
        const float e1 = t.E1(mu, rough);
        const float3 ess = metalWeight * (metalF0 * e1 + (1.0 - metalF0) * t.S5(mu, rough) - metalB * t.S6(mu, rough));
        return ess + msFresnel(metalFavg(), t.E1Avg(rough)) * (1.0 - e1);
    }

    // BSDF x cos(wi).
    float3 eval(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (wo.z <= 0.0 || wi.z <= 0.0) return float3(0.0);
        const float3 m = normalize(wo + wi);
        const float c = dot(wo, m);
        const float ss = ggxSingle(wo, wi, alpha * alpha);
        const float ms = msLobe(wo, wi, t);
        const float e1Avg = t.E1Avg(rough);
        float3 f = float3(0.0);
        if (metalness > 0.0)
        {
            const float3 F = metalWeight * fresnelF82(c, metalF0, metalB);
            f += metalness * (F * ss + msFresnel(metalFavg(), e1Avg) * ms);
        }
        if (metalness < 1.0)
        {
            const float spec = fresnelDielectric(c, eta) * ss + msFresnel(float3(t.FavgD(eta)), e1Avg).x * ms;
            const float3 diffuse = (1.0 - dielectricAlbedo(wo.z, t)) * eonEval(rho, diffRough, wi, wo) * wi.z;
            f += (1.0 - metalness) * (specColor * spec + diffuse);
        }
        return f;
    }

    float3 albedo(float3 wo, OpenPBRTables t)
    {
        if (wo.z <= 0.0) return float3(0.0);
        const float ed = dielectricAlbedo(wo.z, t);
        const float3 dielectric = specColor * ed + (1.0 - ed) * eonAlbedo(rho, diffRough, wo.z);
        return lerp(dielectric, metalAlbedo(wo.z, t), metalness);
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

    float pdf(float3 wo, float3 wi, OpenPBRTables t)
    {
        if (wo.z <= 0.0 || wi.z <= 0.0) return 0.0;
        const float ps = specularShare(wo, t);
        return ps * ggxPdfBounded(wo, wi, alpha) + (1.0 - ps) * eonPdf(wo, wi, diffRough);
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
            const float3 m = ggxSampleBounded(wo, alpha, u.yz);
            s.wi = reflect(-wo, m);
        }
        else
        {
            s.wi = eonSample(wo, diffRough, u.y, u.z).xyz;
            s.lobe = 1u;
        }
        if (s.wi.z <= 0.0) return s;
        s.pdf = ps * ggxPdfBounded(wo, s.wi, alpha) + (1.0 - ps) * eonPdf(wo, s.wi, diffRough);
        if (s.pdf > 0.0) s.weight = eval(wo, s.wi, t) / s.pdf;
        return s;
    }
};
