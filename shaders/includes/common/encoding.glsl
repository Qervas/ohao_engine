// encoding.glsl — GBuffer encoding/decoding + depth linearization
// Shared across: deferred_lighting, ssao, ssgi, ssr, dof_coc, volumetric_scatter
//
// Part of OHAO Engine shader system
// Location: includes/common/encoding.glsl

#ifndef OHAO_COMMON_ENCODING_GLSL
#define OHAO_COMMON_ENCODING_GLSL

// ---------------------------------------------------------------------------
// Octahedron normal encoding/decoding (A2R10G10B10 GBuffer format)
// ---------------------------------------------------------------------------

// Decode octahedron-encoded normal from GBuffer (2-component [0,1] → unit vec3)
vec3 decodeNormalOctahedron(vec2 encoded) {
    vec2 f = encoded * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Encode unit normal to octahedron (unit vec3 → 2-component [0,1])
vec2 encodeNormalOctahedron(vec3 n) {
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    if (n.z < 0.0) {
        p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0,
                                       p.y >= 0.0 ? 1.0 : -1.0);
    }
    return p;
}

// ---------------------------------------------------------------------------
// Depth linearization
// ---------------------------------------------------------------------------

// Linearize depth using projection matrix elements (Vulkan [0,1] depth range)
// Used by: ssgi, volumetric_scatter
float linearizeDepthProj(float d, mat4 projection) {
    return projection[3][2] / (d * projection[2][3] - projection[2][2]);
}

// Linearize depth using near/far planes (Vulkan [0,1] depth range)
// Used by: dof_coc, volumetric_scatter
float linearizeDepthNearFar(float depth, float nearPlane, float farPlane) {
    return nearPlane * farPlane /
           (farPlane - depth * (farPlane - nearPlane));
}

#endif // OHAO_COMMON_ENCODING_GLSL
