#version 450
#extension GL_ARB_separate_shader_objects : enable

// ---------------------------------------------------------------------------
// Foliage fragment shader.
//
// Writes into the GBuffer (LOAD_OP_LOAD) via four color attachments.
// GBuffer layout:
//   attachment 0: R16G16B16A16_SFLOAT — worldPos.xyz + metallic
//   attachment 1: A2R10G10B10_UNORM   — oct-encoded normal.xy + 0 + roughness
//   attachment 2: R8G8B8A8_SRGB       — albedo.rgb + AO
//   attachment 3: R16G16_SFLOAT       — motion vectors
//
// Features:
//   – Hard alpha cutout (discard at alpha < 0.5)
//   – Two-sided normals (back face = flipped normal = grass-light transmission)
//   – Root darkening (AO gradient from tip to root)
//   – Roughness gradient (root coarser, tip slightly smoother)
//   – Subsurface translucency on back faces (brightened, desaturated transmission)
// ---------------------------------------------------------------------------

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec3 inWorldPos;

layout(location = 0) out vec4 outGBuffer0;
layout(location = 1) out vec4 outGBuffer1;
layout(location = 2) out vec4 outGBuffer2;
layout(location = 3) out vec2 outVelocity;

layout(set = 0, binding = 1) uniform sampler2D grassTex;

layout(push_constant) uniform FoliagePC {
    mat4  viewProj;
    vec3  cameraPos;
    float time;
    vec3  windDir;
    float windStrength;
    float pad0;
    float pad1;
    float pad2;
    float pad3;
} pc;

// Octahedron-normal encoding (matches GBufferPass)
vec2 encodeNormal(vec3 n) {
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    return (n.z < 0.0) ? ((1.0 - abs(p.yx)) * sign(p)) : p;
}

void main() {
    vec4 texColor = texture(grassTex, inUV);

    // Hard alpha cutout — no partial transparency in the GBuffer
    if (texColor.a < 0.5) discard;

    vec3 albedo = texColor.rgb * inColor.rgb;

    // ── Two-sided normal ──────────────────────────────────────────────────────
    // For back faces (light transmits through thin blade), flip the normal so
    // lighting simulates forward-scattered transmission rather than shadow.
    vec3 N = normalize(inNormal);
    if (!gl_FrontFacing) {
        N = -N;
        // Translucency: back-lit grass is brighter and slightly desaturated.
        // Mix the albedo toward a warm yellow-green transmission colour.
        float lum  = dot(albedo, vec3(0.299, 0.587, 0.114));
        vec3  trans = mix(albedo, vec3(lum * 1.4, lum * 1.3, lum * 0.8), 0.4);
        albedo = clamp(trans, 0.0, 1.0);
    }

    // ── Root darkening (AO) ───────────────────────────────────────────────────
    // UV.y == 0 → root (darker, shadowed by ground); UV.y == 1 → tip (lighter).
    float rootAO = mix(0.55, 1.0, inUV.y);

    // ── Roughness gradient ────────────────────────────────────────────────────
    // Root is coarser (wet/dirty soil contact); tips are slightly smoother.
    float roughness = mix(0.90, 0.70, inUV.y);

    const float metallic = 0.0;

    outGBuffer0 = vec4(inWorldPos, metallic);
    outGBuffer1 = vec4(encodeNormal(N), 0.0, roughness);
    outGBuffer2 = vec4(albedo, rootAO);
    outVelocity = vec2(0.0);
}
