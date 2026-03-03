#version 450
#extension GL_ARB_separate_shader_objects : enable

// ---------------------------------------------------------------------------
// Foliage vertex shader.
//
// Billboard rendering: each grass clump (FoliageInstance) is drawn as either
// a cross-quad (2 perpendicular quads) or a single quad facing the camera.
//
// Vertex layout (28 bytes per vertex, binding 0):
//   vec3 inPos    – local-space billboard vertex [-0.5..0.5] × [0..1]
//   vec2 inUV     – texture coordinates
//   vec3 inNormal – local-space normal (may be axis-aligned per quad)
// ---------------------------------------------------------------------------

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;

struct FoliageInstance {
    vec3  position;
    float scale;
    vec4  color;
    uint  lod;
    uint  pad0;
    uint  pad1;
    uint  pad2;
};

layout(std430, set = 0, binding = 0) readonly buffer InstanceBuffer {
    FoliageInstance instances[];
};

layout(push_constant) uniform FoliagePC {
    mat4  viewProj;      //  64
    vec3  cameraPos;     //  12
    float time;          //   4  →  80
    vec3  windDir;       //  12
    float windStrength;  //   4  →  96
    float pad0;          //   4
    float pad1;          //   4
    float pad2;          //   4
    float pad3;          //   4  → 112
} pc;

layout(location = 0) out vec2  outUV;
layout(location = 1) out vec3  outNormal;
layout(location = 2) out vec4  outColor;
layout(location = 3) out vec3  outWorldPos;

void main() {
    FoliageInstance inst = instances[gl_InstanceIndex];

    // ── Camera-facing billboard axes ─────────────────────────────────────────
    // Right is always horizontal; up is world +Y so blades stay upright.
    vec3 toCamera = normalize(pc.cameraPos - inst.position);
    vec3 right    = normalize(cross(vec3(0.0, 1.0, 0.0), toCamera));
    vec3 up       = vec3(0.0, 1.0, 0.0);

    // Expand local billboard vertex to world space
    vec3 worldPos = inst.position
                  + right * inPos.x * inst.scale
                  + up    * inPos.y * inst.scale;

    // ── Wind deformation ─────────────────────────────────────────────────────
    // Two overlapping sine waves produce more organic, less mechanical motion:
    //   primary  – slow, large amplitude (main sway)
    //   secondary – fast, smaller amplitude (gust ripple)
    // Both scale by windFactor so the root stays anchored to the ground.
    float windFactor  = inPos.y * inPos.y;  // quadratic: root anchored, tip exaggerated
    float phase       = inst.position.x * 0.5 + inst.position.z * 0.31;

    float primary     = sin(pc.time * 1.8 + phase) * pc.windStrength;
    float secondary   = sin(pc.time * 4.7 + phase * 1.3 + 0.9) * pc.windStrength * 0.3;

    worldPos += pc.windDir * (primary + secondary) * windFactor;

    // ── Outputs ──────────────────────────────────────────────────────────────
    outUV       = inUV;
    // Transform local normal into world space via billboard rotation
    outNormal   = normalize(right * inNormal.x + up * inNormal.y + toCamera * inNormal.z);
    outColor    = inst.color;
    outWorldPos = worldPos;

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
}
