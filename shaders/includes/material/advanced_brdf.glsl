// Advanced BRDF functions for AAA material rendering
// Includes: Clear Coat, Subsurface Scattering, Anisotropy, Sheen, Transmission

#ifndef ADVANCED_BRDF_GLSL
#define ADVANCED_BRDF_GLSL

// Material feature flags (must match C++ MaterialFeatures enum)
#define FEATURE_DOUBLE_SIDED     (1u << 0)
#define FEATURE_ALPHA_TEST       (1u << 1)
#define FEATURE_RECEIVE_SHADOWS  (1u << 2)
#define FEATURE_CAST_SHADOWS     (1u << 3)
#define FEATURE_USE_NORMAL_MAP   (1u << 4)
#define FEATURE_USE_EMISSIVE     (1u << 5)
#define FEATURE_USE_AO           (1u << 6)
#define FEATURE_USE_HEIGHT       (1u << 7)
#define FEATURE_CLEAR_COAT       (1u << 8)
#define FEATURE_SUBSURFACE       (1u << 9)
#define FEATURE_ANISOTROPY       (1u << 10)
#define FEATURE_TRANSMISSION     (1u << 11)
#define FEATURE_SHEEN            (1u << 12)

bool hasFeature(uint features, uint flag) {
    return (features & flag) != 0;
}

// ============================================================================
// Clear Coat (automotive paint, varnished wood)
// Uses a secondary specular lobe with fixed IOR ~1.5
// ============================================================================

// Fresnel for clear coat layer (IOR = 1.5)
float F_ClearCoat(float VdotH) {
    const float f0 = 0.04;  // IOR 1.5
    return f0 + (1.0 - f0) * pow(1.0 - VdotH, 5.0);
}

// GGX distribution for clear coat
float D_ClearCoat(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * denom * denom);
}

// Visibility term for clear coat
float V_ClearCoat(float NdotL, float NdotV, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;

    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);

    return 0.5 / max(GGXL + GGXV, 0.0001);
}

// Full clear coat specular evaluation
vec3 evaluateClearCoat(vec3 N, vec3 V, vec3 L, vec3 H,
                        float clearCoatIntensity, float clearCoatRoughness) {
    if (clearCoatIntensity <= 0.0) return vec3(0.0);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float D = D_ClearCoat(NdotH, max(clearCoatRoughness, 0.045));
    float V_cc = V_ClearCoat(NdotL, NdotV, max(clearCoatRoughness, 0.045));
    float F = F_ClearCoat(VdotH);

    return vec3(D * V_cc * F * clearCoatIntensity);
}

// Attenuation of base layer due to clear coat
float clearCoatAttenuation(float clearCoatIntensity, float NdotV) {
    float Fc = F_ClearCoat(NdotV);
    return 1.0 - clearCoatIntensity * Fc;
}

// ============================================================================
// Subsurface Scattering (skin, wax, leaves, marble)
// Simplified diffuse transmission approximation
// ============================================================================

// Subsurface scattering approximation using curvature
vec3 evaluateSubsurface(vec3 N, vec3 V, vec3 L, vec3 subsurfaceColor,
                         float subsurfaceIntensity, float subsurfaceRadius,
                         vec3 lightColor, float thickness) {
    if (subsurfaceIntensity <= 0.0) return vec3(0.0);

    // Back-lighting term (light scattering through thin geometry)
    float NdotL = dot(N, L);
    float backLighting = max(-NdotL, 0.0);

    // Curvature-based scattering (wrap lighting)
    float wrap = max(0.0, (NdotL + subsurfaceRadius) / (1.0 + subsurfaceRadius));
    float scatter = wrap * wrap;

    // Transmission through material (thinner = more transmission)
    float transmission = exp(-thickness / subsurfaceRadius);

    // Combine terms
    vec3 sss = subsurfaceColor * lightColor * subsurfaceIntensity;
    sss *= scatter + backLighting * transmission;

    return sss;
}

// Pre-integrated subsurface scattering for skin
vec3 evaluateSkinSSS(vec3 N, vec3 L, vec3 subsurfaceColor, float curvature) {
    float NdotL = dot(N, L);

    // Approximate skin BRDF with wrapped lighting
    float diffuse = max(0.0, NdotL * 0.5 + 0.5);

    // Red channel scatters more (blood)
    vec3 scatter = vec3(
        pow(diffuse, 1.0),
        pow(diffuse, 1.3),
        pow(diffuse, 1.8)
    );

    // Curvature affects scatter radius
    scatter = mix(vec3(diffuse), scatter, curvature);

    return scatter * subsurfaceColor;
}

// ============================================================================
// Anisotropic Reflections (brushed metal, hair, carbon fiber)
// ============================================================================

// Anisotropic GGX distribution
float D_GGX_Aniso(float NdotH, float TdotH, float BdotH,
                   float roughnessT, float roughnessB) {
    float a2T = roughnessT * roughnessT;
    float a2B = roughnessB * roughnessB;

    float d = (TdotH * TdotH / a2T) + (BdotH * BdotH / a2B) + NdotH * NdotH;
    return 1.0 / (3.14159265 * roughnessT * roughnessB * d * d);
}

// Anisotropic visibility term
float V_GGX_Aniso(float NdotV, float NdotL, float TdotV, float BdotV,
                   float TdotL, float BdotL, float roughnessT, float roughnessB) {
    float a2T = roughnessT * roughnessT;
    float a2B = roughnessB * roughnessB;

    float lambdaV = NdotL * sqrt(TdotV * TdotV * a2T + BdotV * BdotV * a2B + NdotV * NdotV);
    float lambdaL = NdotV * sqrt(TdotL * TdotL * a2T + BdotL * BdotL * a2B + NdotL * NdotL);

    return 0.5 / max(lambdaV + lambdaL, 0.0001);
}

// Compute anisotropic roughness from base roughness and anisotropy parameter
void computeAnisotropicRoughness(float roughness, float anisotropy,
                                  out float roughnessT, out float roughnessB) {
    float r2 = roughness * roughness;
    float aspect = sqrt(1.0 - anisotropy * 0.9);
    roughnessT = r2 / aspect;
    roughnessB = r2 * aspect;

    // Clamp to minimum roughness
    roughnessT = max(roughnessT, 0.001);
    roughnessB = max(roughnessB, 0.001);
}

// Rotate tangent frame for anisotropy direction
void rotateAnisotropyFrame(float rotation, inout vec3 T, inout vec3 B) {
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 newT = T * c + B * s;
    vec3 newB = B * c - T * s;
    T = newT;
    B = newB;
}

// Full anisotropic specular evaluation
vec3 evaluateAnisotropicSpecular(vec3 N, vec3 V, vec3 L, vec3 H,
                                   vec3 T, vec3 B, vec3 F0,
                                   float roughness, float anisotropy) {
    if (abs(anisotropy) < 0.001) return vec3(0.0);  // No anisotropy

    float roughnessT, roughnessB;
    computeAnisotropicRoughness(roughness, anisotropy, roughnessT, roughnessB);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float TdotH = dot(T, H);
    float BdotH = dot(B, H);
    float TdotL = dot(T, L);
    float BdotL = dot(B, L);
    float TdotV = dot(T, V);
    float BdotV = dot(B, V);

    float D = D_GGX_Aniso(NdotH, TdotH, BdotH, roughnessT, roughnessB);
    float G = V_GGX_Aniso(NdotV, NdotL, TdotV, BdotV, TdotL, BdotL, roughnessT, roughnessB);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

    return D * G * F;
}

// ============================================================================
// Sheen (velvet, fabric, cloth)
// ============================================================================

// Sheen distribution (Charlie distribution)
float D_Charlie(float NdotH, float roughness) {
    float a = roughness * roughness;
    float invR = 1.0 / a;
    float cos2h = NdotH * NdotH;
    float sin2h = 1.0 - cos2h;
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * 3.14159265);
}

// Sheen visibility
float V_Sheen(float NdotL, float NdotV, float roughness) {
    return 1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV));
}

// Sheen color approximation (Filament approach)
vec3 sheenColorFromAlbedo(vec3 albedo) {
    float luminance = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    return luminance < 0.25 ? albedo : vec3(luminance);
}

// Full sheen evaluation
vec3 evaluateSheen(vec3 N, vec3 V, vec3 L, vec3 H,
                    vec3 sheenColor, float sheenIntensity, float sheenRoughness) {
    if (sheenIntensity <= 0.0) return vec3(0.0);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);

    float D = D_Charlie(NdotH, max(sheenRoughness, 0.045));
    float V_s = V_Sheen(NdotL, NdotV, sheenRoughness);

    return sheenColor * D * V_s * sheenIntensity;
}

// ============================================================================
// Transmission (glass, liquids, thin translucent objects)
// ============================================================================

// Refraction with Snell's law
vec3 refractRay(vec3 I, vec3 N, float ior) {
    float NdotI = dot(N, I);
    float eta = NdotI > 0.0 ? ior : 1.0 / ior;
    float k = 1.0 - eta * eta * (1.0 - NdotI * NdotI);

    if (k < 0.0) {
        // Total internal reflection
        return reflect(I, N);
    }

    return eta * I - (eta * NdotI + sqrt(k)) * N;
}

// Fresnel for transmission
float fresnelTransmission(float NdotV, float ior) {
    float f0 = pow((1.0 - ior) / (1.0 + ior), 2.0);
    return f0 + (1.0 - f0) * pow(1.0 - NdotV, 5.0);
}

// Approximate absorption through material
vec3 transmissionAbsorption(vec3 color, float distance, float density) {
    return exp(-density * distance * (vec3(1.0) - color));
}

// Evaluate transmission (simplified, single-scattering)
vec3 evaluateTransmission(vec3 N, vec3 V, vec3 L, vec3 transmissionColor,
                           float transmission, float ior, float roughness,
                           float thickness) {
    if (transmission <= 0.0) return vec3(0.0);

    float NdotV = max(abs(dot(N, V)), 0.001);
    float NdotL = dot(N, L);

    // Fresnel determines reflection vs transmission ratio
    float F = fresnelTransmission(NdotV, ior);
    float transmitted = (1.0 - F) * transmission;

    // Light from opposite side
    float backLighting = max(-NdotL, 0.0);

    // Diffuse transmission through material
    vec3 absorption = transmissionAbsorption(transmissionColor, thickness, 1.0);

    // Roughness affects blur of transmitted light
    float blur = 1.0 / (1.0 + roughness * 4.0);

    return absorption * transmitted * backLighting * blur;
}

// ============================================================================
// Combined Advanced Material Evaluation
// ============================================================================

struct AdvancedMaterialParams {
    vec3 albedo;
    float roughness;
    float metallic;
    vec3 F0;

    float clearCoatIntensity;
    float clearCoatRoughness;

    float subsurfaceIntensity;
    float subsurfaceRadius;
    vec3 subsurfaceColor;

    float anisotropy;
    float anisotropyRotation;

    float sheenIntensity;
    float sheenRoughness;
    vec3 sheenColor;

    float transmission;
    float ior;
    float thickness;

    uint features;
};

vec3 evaluateAdvancedBRDF(AdvancedMaterialParams mat,
                           vec3 N, vec3 V, vec3 L, vec3 H,
                           vec3 T, vec3 B,
                           vec3 lightColor) {
    vec3 result = vec3(0.0);

    // Clear coat (applied on top, attenuates base layer)
    float ccAttenuation = 1.0;
    if (hasFeature(mat.features, FEATURE_CLEAR_COAT)) {
        result += evaluateClearCoat(N, V, L, H, mat.clearCoatIntensity, mat.clearCoatRoughness) * lightColor;
        ccAttenuation = clearCoatAttenuation(mat.clearCoatIntensity, max(dot(N, V), 0.0));
    }

    // Sheen (additive)
    if (hasFeature(mat.features, FEATURE_SHEEN)) {
        result += evaluateSheen(N, V, L, H, mat.sheenColor, mat.sheenIntensity, mat.sheenRoughness) * lightColor;
    }

    // Subsurface scattering (replaces part of diffuse)
    vec3 diffuseContrib = vec3(0.0);
    if (hasFeature(mat.features, FEATURE_SUBSURFACE)) {
        diffuseContrib = evaluateSubsurface(N, V, L, mat.subsurfaceColor,
                                             mat.subsurfaceIntensity, mat.subsurfaceRadius,
                                             lightColor, mat.thickness);
    }

    // Transmission (for glass-like materials)
    if (hasFeature(mat.features, FEATURE_TRANSMISSION)) {
        result += evaluateTransmission(N, V, L, mat.albedo, mat.transmission,
                                        mat.ior, mat.roughness, mat.thickness) * lightColor;
    }

    // Apply clear coat attenuation to diffuse
    result += diffuseContrib * ccAttenuation;

    return result;
}

#endif // ADVANCED_BRDF_GLSL
