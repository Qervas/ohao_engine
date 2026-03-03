#version 450
#extension GL_ARB_separate_shader_objects : enable

// XZ grid passthrough — tessellation stages subdivide and displace.
// Input: flat XZ in [-0.5, 0.5] normalized space (terrainSize is applied in world stage).
layout(location = 0) in vec2 inXZ;

layout(push_constant) uniform TerrainParams {
    mat4    viewProj;      // 64
    vec3    cameraPos;     // 12 → 76
    float   heightScale;   //  4 → 80
    float   terrainSize;   //  4 → 84
    float   snowCover;     //  4 → 88
    float   wetness;       //  4 → 92
    float   time;          //  4 → 96
    float   hmapResInv;    //  4 → 100  (1.0 / heightmap resolution)
    int     terrainType;   //  4 → 104  (0=external, 1-6=procedural types)
    float   pad2;          //  4 → 108
    float   waterLevel;    //  4 → 112  NEW
    float   frostCover;    //  4 → 116  NEW
    float   tileOffsetX;   //  4 → 120  NEW
    float   tileOffsetZ;   //  4 → 124  NEW
} pc;

layout(location = 0) out vec2 outXZ;

void main() {
    outXZ = inXZ;
    // Provisional clip-space position for early culling.
    // The tessellation evaluation stage overwrites gl_Position after heightmap displacement.
    vec3 worldPos = vec3(inXZ.x * pc.terrainSize + pc.tileOffsetX, 0.0,
                         inXZ.y * pc.terrainSize + pc.tileOffsetZ);
    gl_Position   = pc.viewProj * vec4(worldPos, 1.0);
}
