// The display transform (frontier F2 T1): every path that shows a frame (the deferred post's
// tonemap, the path tracer's output, NRD's and DLSS's frames, OIDN's through its C++ twin
// render/display/display.hpp) turns scene-linear radiance into an sRGB-encoded pixel here.
//
//   displayPixel(L) = sRGB(curve(L x exposure)),  exposure = 1 / (1.2 x 2^EV100)
//
// Exposure is a physical camera's (Lagarde and de Rousiers, SIGGRAPH 2014, "Moving Frostbite to
// PBR"): EV100 at ISO 100, the 1.2 the sensor's saturation-based speed factor (78 / (0.65 x 100)).
// The curves:
//   - AgX (Sobotka), the default (ruling F-R4): the base AgX of Blender 4 and Filament, as fit by
//     Wrensch's "Minimal AgX" (2023): the inset matrix, log2 over [-12.47393, 4.026069] EV, a
//     6th-order polynomial sigmoid, the outset matrix. Its output is display-encoded for a 2.2
//     power; it is linearised and encoded again by the exact sRGB function.
//   - Khronos PBR Neutral (2024, the reference code): base colours under unit light show as
//     themselves; for material review and the referee's PNGs.

static const uint kDisplayAgX = 0u;
static const uint kDisplayNeutral = 1u;

float displayExposure(float ev100) { return 1.0 / (1.2 * exp2(ev100)); }

// GLSL-style column-major 3x3: the columns c0, c1, c2 times v.
float3 columns(float3 c0, float3 c1, float3 c2, float3 v) { return c0 * v.x + c1 * v.y + c2 * v.z; }

float3 agxContrast(float3 x)
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// AgX: scene-linear (Rec.709) to linear display light.
float3 agx(float3 c)
{
    const float minEv = -12.47393, maxEv = 4.026069;
    float3 v = columns(float3(0.842479062253094, 0.0423282422610123, 0.0423756549057051),
                       float3(0.0784335999999992, 0.878468636469772, 0.0784336),
                       float3(0.0792237451477643, 0.0791661274605434, 0.879142973793104), max(c, 0.0));
    v = clamp(log2(max(v, 1e-10)), minEv, maxEv);
    v = agxContrast((v - minEv) / (maxEv - minEv));
    v = columns(float3(1.19687900512017, -0.0528968517574562, -0.0529716355144438),
                float3(-0.0980208811401368, 1.15190312990417, -0.0980434501171241),
                float3(-0.0990297440797205, -0.0989611768448433, 1.15107367264116), v);
    return pow(saturate(v), 2.2);
}

// Khronos PBR Neutral: scene-linear to linear display light.
float3 pbrNeutral(float3 c)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    c = max(c, 0.0);
    const float x = min(c.r, min(c.g, c.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    const float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) return c;
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(c, float3(newPeak), g);
}

// The sRGB transfer function (IEC 61966-2-1), linear to encoded.
float3 srgbEncode(float3 c)
{
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float3 srgbDecode(float3 e)
{
    e = saturate(e);
    return select(e <= 0.04045, e / 12.92, pow((e + 0.055) / 1.055, 2.4));
}

// Scene-linear radiance to an sRGB-encoded pixel at a linear exposure scale.
float3 displayPixel(float3 radiance, float exposure, uint curve)
{
    const float3 c = radiance * exposure;
    return srgbEncode(curve == kDisplayNeutral ? pbrNeutral(c) : agx(c));
}
