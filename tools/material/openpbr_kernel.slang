// shaders/material/openpbr.slang on the CPU: slangc compiles this kernel to C++ (-target cpp), and
// tools/material/openpbr_tables (the energy tables) and tests/material/material_tests (the module's
// tests) call it, so they run the module's own code. Outside shaders/: never compiled to SPIR-V.
//
// One item is 8 float4s of `io`:
//   0  x the op
//   1  base_color.rgb, base_weight
//   2  base_metalness, base_diffuse_roughness, specular_weight, specular_roughness
//   3  specular_color.rgb, specular_ior
//   4  wo.xyz (local, z the normal)
//   5  wi.xyz, or the random numbers u.xyz
//   6  the result
//   7  the result's second half
// Ops:
//   0 eval      6.rgb = BSDF x cos
//   1 sample    6 = (wi, pdf), 7 = (weight, lobe)
//   2 pdf       6.x
//   3 albedo    6.rgb
//   4 tables    a bounded-VNDF sample's single-scatter weight w for u.xy, as (w, w (1-c)^5,
//               w c (1-c)^6, w F_dielectric(c, u.z)); 0 below the horizon (c = m.wo, the alpha the
//               material's specular_roughness squared, unclamped)
//   5 fresnel   6.x = the exact dielectric Fresnel at cosine wo.x, eta wo.y
//   6 lobes     the lobes alone, without the cosine, for reciprocity: 6.x GGX single scatter,
//               6.yzw EON at rho = base_color, 7.x the multiple-scattering lobe
import material.openpbr;

RWStructuredBuffer<float4> io;
StructuredBuffer<float> tables;

[shader("compute")]
[numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
    const uint b = id.x * 8u;
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
    const float3 wo = io[b + 4u].xyz;
    const float3 v = io[b + 5u].xyz;
    OpenPBRTables t;
    t.data = tables;
    const OpenPBRBase base = OpenPBRBase(mi);
    float4 r0 = float4(0.0), r1 = float4(0.0);
    if (op == 0u)
    {
        r0.xyz = base.eval(wo, v, t);
    }
    else if (op == 1u)
    {
        const BsdfSample s = base.sample(wo, v, t);
        r0 = float4(s.wi, s.pdf);
        r1 = float4(s.weight, float(s.lobe));
    }
    else if (op == 2u)
    {
        r0.x = base.pdf(wo, v, t);
    }
    else if (op == 3u)
    {
        r0.xyz = base.albedo(wo, t);
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
    else if (op == 5u)
    {
        r0.x = fresnelDielectric(wo.x, wo.y);
    }
    else if (op == 6u)
    {
        r0.x = ggxSingle(wo, v, base.alpha * base.alpha) / v.z;
        r0.yzw = eonEval(mi.baseColor, mi.baseDiffuseRoughness, v, wo);
        r1.x = base.msLobe(wo, v, t) / v.z;
    }
    io[b + 6u] = r0;
    io[b + 7u] = r1;
}
