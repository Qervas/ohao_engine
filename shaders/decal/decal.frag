#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Deferred OBB decal fragment shader.
// For each rasterized pixel of the decal cube:
//   1. Reconstruct world position from depth buffer.
//   2. Project into decal local OBB space.
//   3. Discard if outside [-1,1]^3 OBB.
//   4. Edge-fade alpha near OBB boundaries.
//   5. Projection-angle fade: low weight on near-perpendicular surfaces.
//   6. Sample decal albedo + optional normal map.
//   7. Output blended colour to GBuffer albedo.

layout(location = 0) in flat uint inDecalIndex;

// Write to GBuffer albedo attachment (LOAD_OP_LOAD, src-alpha blending)
layout(location = 0) out vec4 outAlbedo;

struct DecalData {
    mat4  decalMatrix;    // world → decal local [-1,1]^3
    mat4  worldMatrix;    // decal local → world space
    vec4  colorTint;
    uint  albedoIdx;
    uint  normalIdx;
    float opacity;
    float roughnessScale;
};

layout(std430, set = 0, binding = 0) readonly buffer DecalBuffer {
    DecalData decals[];
};

// Depth buffer — nearest sampler, DEPTH_STENCIL_READ_ONLY_OPTIMAL
layout(set = 0, binding = 1) uniform sampler2D depthBuffer;

// Bindless decal texture array (set 1, binding 0)
layout(set = 1, binding = 0) uniform sampler2D decalTextures[];

layout(push_constant) uniform DecalPC {
    mat4  viewProj;
    mat4  invViewProj;
    vec2  screenSize;
    float pad[2];
} pc;

// ─── Depth-derived surface normal ─────────────────────────────────────────────
// Estimates the geometric surface normal from screen-space position derivatives.
// Cheap substitute for reading the GBuffer normal — works well in the interior
// of surfaces, has aliasing only at silhouette edges (acceptable for decals).
vec3 surfaceNormalFromDepth(vec3 worldPos) {
    vec3 dx = dFdx(worldPos);
    vec3 dy = dFdy(worldPos);
    return normalize(cross(dy, dx));
}

void main() {
    // ── Reconstruct world position from depth ────────────────────────────────
    vec2  uv    = gl_FragCoord.xy / pc.screenSize;
    float depth = texture(depthBuffer, uv).r;

    // Sky / far plane — no surface to project on
    if (depth >= 1.0) discard;

    vec4 ndc    = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldH = pc.invViewProj * ndc;
    vec3 worldPos = worldH.xyz / worldH.w;

    // ── Project into decal OBB local space ──────────────────────────────────
    DecalData d   = decals[inDecalIndex];
    vec4 localH   = d.decalMatrix * vec4(worldPos, 1.0);
    vec3 local    = localH.xyz;

    // OBB bounds check — discard pixels outside the box
    if (any(greaterThan(abs(local), vec3(1.0)))) discard;

    // ── Projection angle fade ────────────────────────────────────────────────
    // Decal projects along its local -Z axis (world space: column 2 of worldMatrix).
    // On near-perpendicular surfaces the decal stretches badly — fade it out.
    vec3 decalDir = normalize(d.worldMatrix[2].xyz);
    vec3 surfN    = surfaceNormalFromDepth(worldPos);
    float projDot = abs(dot(surfN, decalDir));
    // projDot = 1 → perfect face-on, = 0 → edge-on (stretched)
    // Fade in [0.15, 0.40]: below 0.15 is nearly invisible anyway
    float angleFade = smoothstep(0.15, 0.40, projDot);
    if (angleFade < 0.01) discard;

    // ── Smooth edge fade ─────────────────────────────────────────────────────
    // Fade out near all 6 faces of the OBB so seams are never a hard line.
    // Transition region: inner 70% → full opacity, outer 30% → fades to 0.
    float edgeMax   = max(max(abs(local.x), abs(local.y)), abs(local.z));
    float edgeFade  = 1.0 - smoothstep(0.70, 1.0, edgeMax);

    // ── Decal UV: map local XY [-1,1] → [0,1] ───────────────────────────────
    // The decal projects along Z; XY gives the texture coordinates.
    vec2 decalUV = local.xy * 0.5 + 0.5;

    // ── Sample decal albedo ──────────────────────────────────────────────────
    uint texIdx = d.albedoIdx;
    vec4 col;
    if (texIdx == 0xFFFFFFFFu) {
        col = d.colorTint;
    } else {
        col = texture(decalTextures[nonuniformEXT(texIdx)], decalUV) * d.colorTint;
    }

    float alpha = col.a * d.opacity * edgeFade * angleFade;
    if (alpha < 0.01) discard;

    // Output: pre-multiplied colour for src-alpha blending onto GBuffer albedo
    outAlbedo = vec4(col.rgb, alpha);
}
