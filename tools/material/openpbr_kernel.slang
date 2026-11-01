// shaders/material/openpbr.slang on the CPU: slangc compiles this kernel to C++ (-target cpp), and
// tools/material/openpbr_tables (the energy tables) and tests/material/material_tests (the module's
// tests) call it, so they run the module's own code. Outside shaders/: never compiled to SPIR-V.
//
// One item is 14 float4s of `io`:
//   0  x the op
//   1  base_color.rgb, base_weight
//   2  base_metalness, base_diffuse_roughness, specular_weight, specular_roughness
//   3  specular_color.rgb, specular_ior
//   4  wo.xyz (local, z the normal)
//   5  wi.xyz, or the random numbers u.xyz
//   6  the result
//   7  the result's second half
//   8  coat_weight, coat_roughness, coat_roughness_anisotropy, coat_ior
//   9  coat_color.rgb, coat_darkening
//   10 fuzz_color.rgb, fuzz_weight
//   11 fuzz_roughness, specular_roughness_anisotropy
//   12 transmission_color.rgb, transmission_weight
//   13 geometry_thin_walled (0 or 1), thin_film_weight, thin_film_thickness, thin_film_ior
// Ops:
// Ops 0-3 are the whole surface (OpenPBRSurface: fuzz over coat over the base).
//   0 eval      6.rgb = BSDF x cos
//   1 sample    6 = (wi, pdf), 7 = (weight, lobe)
//   2 pdf       6.x
//   3 albedo    6.rgb
//   4 tables    a bounded-VNDF sample's single-scatter weight w for u.xy, as (w, w (1-c)^5,
//               w c (1-c)^6, w F_dielectric(c, u.z)); 0 below the horizon (c = m.wo, the alpha the
//               material's specular_roughness squared, unclamped)
//   5 fresnel   6.x = the exact dielectric Fresnel at cosine wo.x, eta wo.y
//   6 lobes     the lobes alone, without the cosine, for reciprocity: 6.x GGX single scatter,
//               (anisotropic when the material is), 6.yzw EON at rho = base_color, 7.x the
//               multiple-scattering lobe, 7.y the coat's lobe
//   7 aniso     an anisotropic bounded-VNDF sample's single-scatter weight w for u.xy at the full wo
//               (the material's specular_roughness and specular_roughness_anisotropy), as (w,
//               w (1-c)^5, w c (1-c)^6); 0 below the horizon
//   8 glass     a visible normal for u.xy at wo (unbounded VNDF, the material's specular_roughness
//               squared), at relative IOR u.z: (the refraction's (1 - F) G2 / G1, 0 at total
//               internal reflection; the reflection's F G2 / G1, 0 below the horizon)
//   9 film      6.rgb = the thin film's Airy reflectance at cosine wo.x, film IOR wo.y, thickness
//               wo.z (nm), over a base of normal-incidence reflectance v.xyz
//   10 ior      6.x = dispersedIor(wo.x, wo.y, wo.z, v.x)
import material.openpbr;

RWStructuredBuffer<float4> io;
StructuredBuffer<float> tables;

[shader("compute")]
[numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
    const uint b = id.x * 14u;
    const uint op = uint(io[b].x + 0.5);
    MaterialInputs mi;
    mi.baseColor = io[b + 1u].xyz;
    mi.baseWeight = io[b + 1u].w;
    mi.baseMetalness = io[b + 2u].x;
    mi.baseDiffuseRoughness = io[b + 2u].y;
    mi.specularWeight = io[b + 2u].z;
    mi.specularRoughness = io[b + 2u].w;
    mi.specularColor = io[b + 3u].xyz;
    mi.specularIor = io[b + 3u].w;
    mi.coatWeight = io[b + 8u].x;
    mi.coatRoughness = io[b + 8u].y;
    mi.coatRoughnessAnisotropy = io[b + 8u].z;
    mi.coatIor = io[b + 8u].w;
    mi.coatColor = io[b + 9u].xyz;
    mi.coatDarkening = io[b + 9u].w;
    mi.fuzzColor = io[b + 10u].xyz;
    mi.fuzzWeight = io[b + 10u].w;
    mi.fuzzRoughness = io[b + 11u].x;
    mi.specularRoughnessAnisotropy = io[b + 11u].y;
    mi.transmissionColor = io[b + 12u].xyz;
    mi.transmissionWeight = io[b + 12u].w;
    mi.thinWalled = io[b + 13u].x > 0.5;
    mi.thinFilmWeight = io[b + 13u].y;
    mi.thinFilmThickness = io[b + 13u].z;
    mi.thinFilmIor = io[b + 13u].w;
    const float3 wo = io[b + 4u].xyz;
    const float3 v = io[b + 5u].xyz;
    OpenPBRTables t;
    t.data = tables;
    const OpenPBRBase base = OpenPBRBase(mi);
    const OpenPBRSurface surface = OpenPBRSurface(mi);
    float4 r0 = float4(0.0), r1 = float4(0.0);
    if (op == 0u)
    {
        r0.xyz = surface.eval(wo, v, t);
    }
    else if (op == 1u)
    {
        const BsdfSample s = surface.sample(wo, v, t);
        r0 = float4(s.wi, s.pdf);
        r1 = float4(s.weight, float(s.lobe));
    }
    else if (op == 2u)
    {
        r0.x = surface.pdf(wo, v, t);
    }
    else if (op == 3u)
    {
        r0.xyz = surface.albedo(wo, t);
    }
    else if (op == 4u)
    {
        const float alpha = mi.specularRoughness * mi.specularRoughness;
        const float3 m = ggxSampleBounded(wo, alpha, v.xy);
        const float3 wi = reflect(-wo, m);
        if (wi.z > 0.0)
        {
            const float pdf = ggxPdfBounded(wo, wi, alpha);
            const float w = pdf > 0.0 ? ggxSingle(wo, wi, alpha * alpha) / pdf : 0.0;
            const float c = saturate(dot(wo, m));
            const float m1 = 1.0 - c;
            const float m5 = m1 * m1 * m1 * m1 * m1;
            r0 = float4(w, w * m5, w * c * m5 * m1, w * fresnelDielectric(c, v.z));
        }
    }
    else if (op == 7u)
    {
        const float2 a = ggxAnisoAlpha(mi.specularRoughness, mi.specularRoughnessAnisotropy);
        const float3 m = ggxSampleBoundedAniso(wo, a, v.xy);
        const float3 wi = reflect(-wo, m);
        if (wi.z > 0.0)
        {
            const float pdf = ggxPdfBoundedAniso(wo, wi, a);
            const float w = pdf > 0.0 ? ggxSingleAniso(wo, wi, a) / pdf : 0.0;
            const float c = saturate(dot(wo, m));
            const float m1 = 1.0 - c;
            const float m5 = m1 * m1 * m1 * m1 * m1;
            r0.xyz = float3(w, w * m5, w * c * m5 * m1);
        }
    }
    else if (op == 5u)
    {
        r0.x = fresnelDielectric(wo.x, wo.y);
    }
    else if (op == 8u)
    {
        const float alpha = mi.specularRoughness * mi.specularRoughness;
        const float2 a = float2(alpha, alpha);
        const float3 m = ggxSampleVisible(wo, a, v.xy);
        const float g1 = 1.0 / (1.0 + ggxLambdaAniso(wo, a));
        const float F = fresnelDielectric(dot(wo, m), v.z);
        float3 wt;
        if (refractThrough(wo, m, v.z, wt) && wt.z < 0.0)
        {
            const float g2 = 1.0 / (1.0 + ggxLambdaAniso(wo, a) + ggxLambdaAniso(float3(wt.xy, -wt.z), a));
            r0.x = (1.0 - F) * g2 / g1;
        }
        const float3 wr = reflect(-wo, m);
        if (wr.z > 0.0)
        {
            const float g2 = 1.0 / (1.0 + ggxLambdaAniso(wo, a) + ggxLambdaAniso(wr, a));
            r0.y = F * g2 / g1;
        }
    }
    else if (op == 9u)
    {
        r0.xyz = airyReflectance(wo.x, wo.y, wo.z, v);
    }
    else if (op == 10u)
    {
        r0.x = dispersedIor(wo.x, wo.y, wo.z, v.x);
    }
    else if (op == 6u)
    {
        r0.x = (base.anisotropy > 0.0 ? ggxSingleAniso(wo, v, base.alphaXY) : ggxSingle(wo, v, base.alpha * base.alpha)) / v.z;
        r0.yzw = eonEval(mi.baseColor, mi.baseDiffuseRoughness, v, wo);
        r1.x = base.msLobe(wo, v, t) / v.z;
        r1.y = surface.coat.eval(wo, v, t) / v.z;
    }
    io[b + 6u] = r0;   // (the material rows are inputs only)
    io[b + 7u] = r1;
}
