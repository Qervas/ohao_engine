// A G-buffer pixel's material as the deferred frame shades it: OpenPBR's base inputs from the
// G-buffer's targets, with the weather (wetness, snow, frost, mud) applied. The lighting pass
// (core/deferred_lighting.slang) and the direct light's resampling (rt/restir_di.slang) both read
// it, so the light a reservoir aims for is the light lighting shades.
import shared.layout;
import material.openpbr;
import material.material_layers;

struct Weather
{
    float wetness;
    float snowCover;
    float frostCover;
};

float3 decodeNormalOctahedron(float2 e)
{
    const float2 f = e * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// OpenPBR's layers from the G-buffer's layers target and the pixel's material's rows 7-10 (`rows` the
// material table): into mi, and the anisotropic lobes' tangent (decoded from N), 0 when none.
float3 gbufferLayers(inout MaterialInputs mi, uint2 layers, StructuredBuffer<float4> rows, float3 N)
{
    if (layers.y == 0u && ((layers.x >> 12u) & 255u) == 0u) return float3(0.0);   // no coat, fuzz or anisotropy
    const uint r = layersMaterial(layers) * kMaterialRows + kLayerRowFirst;
    const float3 T = unpackLayers(layers, mi, rows[r], rows[r + 1u], rows[r + 2u], rows[r + 3u], N);
    // The thin film (step 3, row 14; an opaque G-buffer surface has no transmission).
    const float4 film = rows[layersMaterial(layers) * kMaterialRows + kTransmissionRowFirst + 2u];
    mi.thinFilmWeight = film.x;
    mi.thinFilmThickness = film.y;
    mi.thinFilmIor = film.z;
    return mi.specularRoughnessAnisotropy > 0.0 || (mi.coatWeight > 0.0 && mi.coatRoughnessAnisotropy > 0.0) ? T : float3(0.0);
}

// g0 (position, metallic), g1 (normal, roughness), g2 (albedo, AO), spec (specular_color,
// specular_weight / 1.5) and pbr (base_diffuse_roughness, (specular_ior - 1) / 2.55); N the decoded
// normal. Weather: wetness darkens and smooths faces looking up, snow and frost coat them, mud tints.
MaterialInputs gbufferMaterial(float4 g0, float4 g1, float4 g2, float4 spec, float2 pbr, float3 N, Weather w)
{
    float metallic = g0.a;
    float roughness = g1.b;
    float3 albedo = g2.rgb;
    if (w.wetness > 0.001)
    {
        const float k = w.wetness * saturate(N.y * 2.0);
        albedo *= 1.0 - k * 0.30;
        roughness = lerp(roughness, max(roughness * 0.15, 0.02), k);
    }
    if (w.snowCover > 0.001)
    {
        const float s = w.snowCover * saturate((N.y - 0.3) * (1.0 / 0.7));
        albedo = lerp(albedo, float3(0.82, 0.87, 0.92), s);
        roughness = lerp(roughness, 0.90, s);
        metallic = lerp(metallic, 0.0, s);
    }
    if (w.wetness > 0.5)
    {
        const float k = w.wetness * saturate(N.y * 2.0);
        const float mud = saturate((k - 0.5) * 2.0) * (1.0 - metallic);
        albedo = lerp(albedo, albedo * float3(0.35, 0.27, 0.18), mud * 0.55);
    }
    if (w.frostCover > 0.001)
    {
        const float f = w.frostCover;
        albedo = lerp(albedo, float3(0.78, 0.88, 0.95), f * 0.50);
        roughness = lerp(roughness, 0.08, f * 0.70);
        metallic = lerp(metallic, 0.10, f * 0.40);
    }

    MaterialInputs mi = defaultMaterialInputs();
    mi.baseColor = albedo;   // base_weight is folded in by the G-buffer
    mi.baseWeight = 1.0;
    mi.baseMetalness = metallic;
    mi.baseDiffuseRoughness = pbr.r;
    mi.specularWeight = spec.a * 1.5;
    mi.specularColor = spec.rgb;
    mi.specularRoughness = roughness;
    mi.specularIor = 1.0 + pbr.g * 2.55;
    return mi;
}
