// NRD REBLUR front/back-end helpers (ported from NRD.hlsli v4.17).
// Radiance AOVs must be YCoCg + normalized hit-distance before REBLUR;
// compose must unpack YCoCg → linear after REBLUR.

#ifndef OHAO_NRD_FRONTEND_GLSL
#define OHAO_NRD_FRONTEND_GLSL

// Default nrd::ReblurHitDistanceParameters { A=3, B=0.1, C=20 }
const vec3 NRD_HIT_DIST_PARAMS = vec3(3.0, 0.1, 20.0);

vec3 nrdLinearToYCoCg(vec3 color) {
    float Y  = dot(color, vec3(0.25, 0.5, 0.25));
    float Co = dot(color, vec3(0.5, 0.0, -0.5));
    float Cg = dot(color, vec3(-0.25, 0.5, -0.25));
    return vec3(Y, Co, Cg);
}

vec3 nrdYCoCgToLinear(vec3 color) {
    float t = color.x - color.z;
    vec3 r;
    r.y = color.x + color.z;
    r.x = t + color.y;
    r.z = t - color.y;
    return max(r, vec3(0.0));
}

// saturate(hitDist / f) with f = (A + |viewZ|*B) * mix(C, 1, smc(roughness))
float nrdNormHitDist(float hitDist, float viewZ, float roughness) {
    float smc = roughness * roughness; // lobe-spread proxy (NRD uses a richer F)
    smc = clamp(smc, 0.0, 1.0);
    float f = (NRD_HIT_DIST_PARAMS.x + abs(viewZ) * NRD_HIT_DIST_PARAMS.y)
            * mix(NRD_HIT_DIST_PARAMS.z, 1.0, smc);
    return clamp(hitDist / max(f, 1e-6), 0.0, 1.0);
}

// Pack for IN_DIFF/SPEC_RADIANCE_HITDIST
vec4 nrdPackRadianceHitDist(vec3 linearRadiance, float hitDist, float viewZ, float roughness) {
    float normHd = nrdNormHitDist(max(hitDist, 0.0), viewZ, roughness);
    vec3 ycocg = nrdLinearToYCoCg(max(linearRadiance, vec3(0.0)));
    return vec4(ycocg, normHd);
}

#endif
