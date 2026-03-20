#version 450
#extension GL_ARB_separate_shader_objects : enable

// Tessellation Evaluation Shader — quad domain, smooth spacing, CCW winding
layout(quads, fractional_even_spacing, ccw) in;

layout(set = 0, binding = 0) uniform sampler2D heightmap;

layout(push_constant) uniform TerrainParams {
    mat4    viewProj;
    vec3    cameraPos;
    float   heightScale;
    float   terrainSize;
    float   snowCover;
    float   wetness;
    float   time;
    float   hmapResInv;   // 1.0 / heightmapResolution (replaces hardcoded 1/512)
    int     terrainType;
    float   pad2;
    float   waterLevel;    //  4 → 112  NEW
    float   frostCover;    //  4 → 116  NEW
    float   tileOffsetX;   //  4 → 120  NEW
    float   tileOffsetZ;   //  4 → 124  NEW
} pc;

layout(location = 0) in  vec2 inXZ[];     // per control point (normalized [-0.5, 0.5])
layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;

void main() {
    // Bilinear interpolation of the four patch corner XZ positions
    // Control point order: [0]=BL, [1]=BR, [2]=TR, [3]=TL  (CCW quad domain)
    vec2 u   = gl_TessCoord.xy;
    vec2 xzBL = inXZ[0], xzBR = inXZ[1], xzTR = inXZ[2], xzTL = inXZ[3];

    vec2 xz = mix(mix(xzBL, xzBR, u.x), mix(xzTL, xzTR, u.x), u.y);

    // UV in [0, 1] (xz lives in [-0.5, 0.5])
    vec2 uv   = xz + 0.5;
    outUV     = uv;

    // Sample heightmap and displace
    float H      = texture(heightmap, uv).r;
    float height = H * pc.heightScale;
    vec3 worldPos = vec3(xz.x * pc.terrainSize + pc.tileOffsetX, height,
                         xz.y * pc.terrainSize + pc.tileOffsetZ);
    outWorldPos   = worldPos;

    // Analytic normal via central-difference finite differences.
    // Uses pc.hmapResInv so resolution is dynamic (not hardcoded to 1/512).
    float ts = pc.hmapResInv;
    float hL = texture(heightmap, uv + vec2(-ts, 0.0)).r * pc.heightScale;
    float hR = texture(heightmap, uv + vec2( ts, 0.0)).r * pc.heightScale;
    float hD = texture(heightmap, uv + vec2(0.0, -ts)).r * pc.heightScale;
    float hU = texture(heightmap, uv + vec2(0.0,  ts)).r * pc.heightScale;
    float stepXZ = ts * pc.terrainSize;

    vec3 tangentX = normalize(vec3(stepXZ * 2.0, hR - hL, 0.0));
    vec3 tangentZ = normalize(vec3(0.0, hU - hD, stepXZ * 2.0));
    outNormal     = normalize(cross(tangentZ, tangentX));

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
}
