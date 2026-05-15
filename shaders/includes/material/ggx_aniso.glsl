// Sub-plan 4.K: anisotropic-aware GGX D term.
//
// Used by the path-tracer raygen for direct-light NEE, env-MIS, and BSDF
// importance-sampling PDF computation. When anisotropy <= 0, returns the
// isotropic GGX formula (numerically equivalent to the inline form so
// pre-existing rendered references stay bit-for-bit identical at aniso=0).
//
// Tangent frame is reconstructed from the shading normal via Frisvad's basis
// (no geometric tangent attribute needed). This gives a consistent — though
// arbitrary — anisotropy direction; rotation can re-orient it per-shot via
// pc.jitter.w.

#ifndef GGX_ANISO_GLSL
#define GGX_ANISO_GLSL

// Coherent tangent field anchored to world-up. Frisvad's basis is fine for any
// math that doesn't care about direction (e.g. importance sampling) but it
// varies discontinuously with N — neighbouring pixels on a sphere see totally
// different T,B, so the anisotropy direction scrambles and the highlight
// averages out into something that looks isotropic. We project world-up onto
// the surface plane to get a tangent that smoothly varies across the surface
// (looks like a lathe-turned/rotational brush on spheres, brushed-along-grain
// on planes). Falls back to world-right at the poles where up ~ N.
void worldUpTangent(vec3 n, out vec3 t, out vec3 b) {
    vec3 up  = vec3(0.0, 1.0, 0.0);
    vec3 ref = abs(dot(up, n)) > 0.97 ? vec3(1.0, 0.0, 0.0) : up;
    t = normalize(ref - n * dot(ref, n));
    b = cross(n, t);
}

float ggxD_anisoOrIso(vec3 N, vec3 H, float NdotH, float roughness,
                      float anisotropy, float rotation) {
    if (anisotropy < 0.001) {
        float a     = roughness * roughness;
        float a2    = a * a;
        float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
        return a2 / (3.14159265 * denom * denom + 0.0001);
    }
    vec3 T, B;
    worldUpTangent(N, T, B);
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 Tr = T * c + B * s;
    vec3 Br = B * c - T * s;

    float r2     = roughness * roughness;
    float aspect = sqrt(1.0 - anisotropy * 0.9);
    float rT     = max(r2 / aspect, 0.001);
    float rB     = max(r2 * aspect, 0.001);

    float TdotH = dot(Tr, H);
    float BdotH = dot(Br, H);
    float d     = (TdotH * TdotH / rT) + (BdotH * BdotH / rB) + NdotH * NdotH;
    return 1.0 / (3.14159265 * rT * rB * d * d + 0.0001);
}

#endif // GGX_ANISO_GLSL
