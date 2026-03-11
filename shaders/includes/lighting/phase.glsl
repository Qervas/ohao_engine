// phase.glsl — Scattering phase functions for participating media
// Shared across: cloud.comp, volumetric_scatter.comp
//
// Part of OHAO Engine shader system
// Location: includes/lighting/phase.glsl

#ifndef OHAO_LIGHTING_PHASE_GLSL
#define OHAO_LIGHTING_PHASE_GLSL

#ifndef PI
#define PI 3.14159265359
#endif

// ---------------------------------------------------------------------------
// Henyey-Greenstein phase function
// cosTheta: cos(angle between view and light), g: asymmetry [-1..1]
// g > 0 → forward scattering, g < 0 → back scattering
// ---------------------------------------------------------------------------
float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 0.0001), 1.5));
}

// ---------------------------------------------------------------------------
// Dual-lobe Henyey-Greenstein (silver lining + back-lit edges)
// gFwd: forward lobe asymmetry (0.6–0.85)
// gBack: backward lobe asymmetry (-0.1 to -0.3)
// backRatio: weight of backward lobe (0.2–0.4)
// ---------------------------------------------------------------------------
float dualLobeHG(float cosTheta, float gFwd, float gBack, float backRatio) {
    return mix(henyeyGreenstein(cosTheta, gFwd),
               henyeyGreenstein(cosTheta, gBack),
               backRatio);
}

// ---------------------------------------------------------------------------
// Mie-Schlick approximation (faster than HG, similar shape)
// ---------------------------------------------------------------------------
float mieSchlick(float cosTheta, float g) {
    float k = 1.55 * g - 0.55 * g * g * g;
    float denom = 1.0 + k * cosTheta;
    return (1.0 - k * k) / (4.0 * PI * denom * denom);
}

// ---------------------------------------------------------------------------
// Rayleigh phase function (for atmospheric scattering)
// ---------------------------------------------------------------------------
float rayleighPhase(float cosTheta) {
    return (3.0 / (16.0 * PI)) * (1.0 + cosTheta * cosTheta);
}

#endif // OHAO_LIGHTING_PHASE_GLSL
