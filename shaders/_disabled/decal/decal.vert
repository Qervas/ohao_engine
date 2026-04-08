#version 450

// Deferred OBB decal vertex shader.
// Each decal is rendered as a unit cube in [-1,1]^3.
// The per-decal world matrix (from the SSBO) transforms it into world space.
// gl_InstanceIndex indexes the active decal in the SSBO.

layout(location = 0) in vec3 inPos;

// Per-decal data (indexed by gl_InstanceIndex)
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

layout(push_constant) uniform DecalPC {
    mat4  viewProj;
    mat4  invViewProj;
    vec2  screenSize;
    float pad[2];
} pc;

layout(location = 0) out flat uint outDecalIndex;

void main() {
    uint idx     = uint(gl_InstanceIndex);
    vec3 worldPos = (decals[idx].worldMatrix * vec4(inPos, 1.0)).xyz;
    outDecalIndex = idx;
    gl_Position   = pc.viewProj * vec4(worldPos, 1.0);
}
