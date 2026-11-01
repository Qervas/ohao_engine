// OpenPBR step 2's rows of the material table (shaders/shared/layout.json, kMaterialRows): the coat,
// the fuzz and specular anisotropy, resolved at a uv. Every reader of a material imports it (the
// G-buffer, the path tracer's closest hit, RT GI's hits, the transparent pass), so a material's
// layers are the same wherever it is shaded. The textures are glTF's: KHR_materials_clearcoat's
// factor in R and roughness in G, KHR_materials_sheen's colour in RGB and roughness in A.
//
//   row 7   coat_color.rgb, coat_weight
//   row 8   coat_roughness, coat_roughness_anisotropy, coat_ior, coat_darkening
//   row 9   fuzz_color.rgb, fuzz_weight
//   row 10  fuzz_roughness, specular_roughness_anisotropy, geometry_tangent_rotation (turns), 0
//   row 11  the coat's texture, the coat roughness's, the fuzz colour's, the fuzz roughness's (indices;
//           0xFFFFFFFF none)
//   row 12  transmission_color.rgb, transmission_weight                        (step 3)
//   row 13  transmission_depth, geometry_thin_walled, transmission_dispersion_scale,
//           transmission_dispersion_abbe_number (the volume and dispersion: the path tracer's)
//   row 14  thin_film_weight, thin_film_thickness, thin_film_ior, 0
import material.openpbr;

public static const uint kLayerRowFirst = 7u;

// A reader's bindless textures: each shader samples its own way (the G-buffer with derivatives, a
// ray at mip 0).
public interface IMaterialTextures
{
    public float4 sample(uint index, float2 uv);
}

// The step's parameters from rows 7-11 into m (its base already resolved), the textures applied.
// A material without coat, fuzz or anisotropy reads no texture.
public void applyLayerRows<T : IMaterialTextures>(inout MaterialInputs m, float4 r7, float4 r8, float4 r9, float4 r10,
                                                  float4 r11, float2 uv, T tex)
{
    m.coatColor = r7.rgb;
    m.coatWeight = r7.w;
    m.coatRoughness = r8.x;
    m.coatRoughnessAnisotropy = r8.y;
    m.coatIor = r8.z;
    m.coatDarkening = r8.w;
    m.fuzzColor = r9.rgb;
    m.fuzzWeight = r9.w;
    m.fuzzRoughness = r10.x;
    m.specularRoughnessAnisotropy = r10.y;
    const uint4 t = asuint(r11);
    if (m.coatWeight > 0.0)
    {
        if (t.x < 4096u) m.coatWeight *= tex.sample(t.x, uv).r;
        if (t.y < 4096u) m.coatRoughness *= tex.sample(t.y, uv).g;
    }
    if (m.fuzzWeight > 0.0)
    {
        if (t.z < 4096u) m.fuzzColor *= tex.sample(t.z, uv).rgb;
        if (t.w < 4096u) m.fuzzRoughness *= tex.sample(t.w, uv).a;
    }
}

// Step 3's rows 12-14 into m: transmission, thin-walled, the thin film. (The volume's depth and
// dispersion are not MaterialInputs: the path tracer reads them from row 13 itself.)
public static const uint kTransmissionRowFirst = 12u;

public void applyTransmissionRows(inout MaterialInputs m, float4 r12, float4 r13, float4 r14)
{
    m.transmissionColor = r13.x > 0.0 ? float3(1.0) : r12.rgb;   // at a depth the volume absorbs instead
    m.transmissionWeight = r12.w;
    m.thinWalled = r13.y > 0.5;
    m.thinFilmWeight = r14.x;
    m.thinFilmThickness = r14.y;
    m.thinFilmIor = r14.z;
}

// The anisotropic lobes' tangent: the mesh's tangent T (unit, in the plane of N) turned about N by
// geometry_tangent_rotation turns. OpenPBR's NDF is stretched along it.
public float3 rotatedTangent(float3 N, float3 T, float turns)
{
    if (turns == 0.0) return T;
    const float a = turns * 6.28318530718;
    return T * cos(a) + cross(N, T) * sin(a);
}

// A basis of the plane of N made from N alone (Duff et al. 2017): the G-buffer stores a tangent as
// its angle in it, which lighting reads back from the same (decoded) normal.
public void normalBasis(float3 n, out float3 b1, out float3 b2)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float b = n.x * n.y * a;
    b1 = float3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    b2 = float3(b, s + n.y * n.y * a, -n.y);
}

// The G-buffer's layers target (RG32_UINT, kLayers): word 0 the material's index (12 bits), the
// anisotropy (8) and the tangent's angle in normalBasis over [0, pi) (12; the NDF is the same
// along -T); word 1 the coat's weight and roughness and the fuzz's weight and roughness (8 each),
// textures applied. A pixel's other layer parameters come from the material table by its index.
public uint2 packLayers(uint material, MaterialInputs m, float3 N, float3 T)
{
    float3 b1, b2;
    normalBasis(N, b1, b2);
    float angle = atan2(dot(T, b2), dot(T, b1));
    if (angle < 0.0) angle += 3.14159265359;
    const uint a12 = min(uint(angle * (4096.0 / 3.14159265359) + 0.5), 4095u) & 4095u;
    const uint aniso = uint(saturate(m.specularRoughnessAnisotropy) * 255.0 + 0.5);
    const uint w0 = (material & 4095u) | (aniso << 12u) | (a12 << 20u);
    const uint w1 = uint(saturate(m.coatWeight) * 255.0 + 0.5) | (uint(saturate(m.coatRoughness) * 255.0 + 0.5) << 8u)
                  | (uint(saturate(m.fuzzWeight) * 255.0 + 0.5) << 16u) | (uint(saturate(m.fuzzRoughness) * 255.0 + 0.5) << 24u);
    return uint2(w0, w1);
}

public uint layersMaterial(uint2 p) { return p.x & 4095u; }

// What packLayers stored, back into m (rows 7-10 of the pixel's material for the rest), and the
// tangent from N.
public float3 unpackLayers(uint2 p, inout MaterialInputs m, float4 r7, float4 r8, float4 r9, float4 r10, float3 N)
{
    m.coatColor = r7.rgb;
    m.coatRoughnessAnisotropy = r8.y;
    m.coatIor = r8.z;
    m.coatDarkening = r8.w;
    m.fuzzColor = r9.rgb;
    m.specularRoughnessAnisotropy = float((p.x >> 12u) & 255u) / 255.0;
    m.coatWeight = float(p.y & 255u) / 255.0;
    m.coatRoughness = float((p.y >> 8u) & 255u) / 255.0;
    m.fuzzWeight = float((p.y >> 16u) & 255u) / 255.0;
    m.fuzzRoughness = float(p.y >> 24u) / 255.0;
    float3 b1, b2;
    normalBasis(N, b1, b2);
    const float angle = float(p.x >> 20u) * (3.14159265359 / 4096.0);
    return b1 * cos(angle) + b2 * sin(angle);
}
