#version 450
#extension GL_ARB_separate_shader_objects : enable

// Tessellation Control Shader — 4 control points per patch (quads)
// Adds frustum-culling: patches fully outside the view frustum get level 0
// so the GPU discards them before tessellation.
layout(vertices = 4) out;

layout(push_constant) uniform TerrainParams {
    mat4    viewProj;
    vec3    cameraPos;
    float   heightScale;
    float   terrainSize;
    float   snowCover;
    float   wetness;
    float   time;
    float   hmapResInv;   // 1.0 / heightmapResolution
    int     terrainType;
    float   pad2;
    float   waterLevel;    //  4 → 112  NEW
    float   frostCover;    //  4 → 116  NEW
    float   tileOffsetX;   //  4 → 120  NEW
    float   tileOffsetZ;   //  4 → 124  NEW
} pc;

layout(location = 0) in  vec2 inXZ[];
layout(location = 0) out vec2 outXZ[];

// ─── Frustum plane extraction (Gribb-Hartmann) ───────────────────────────────
// Extracts 6 world-space planes from a column-major view-projection matrix.
// Result planes are unnormalized (ax+by+cz+d); signed distance = dot(p, n)+d.
void extractFrustumPlanes(mat4 m, out vec4 planes[6]) {
    // Rows of the matrix (GLSL stores column-major: m[col][row])
    vec4 r0 = vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
    vec4 r1 = vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
    vec4 r2 = vec4(m[0][2], m[1][2], m[2][2], m[3][2]);
    vec4 r3 = vec4(m[0][3], m[1][3], m[2][3], m[3][3]);

    planes[0] = r3 + r0;  // left
    planes[1] = r3 - r0;  // right
    planes[2] = r3 + r1;  // bottom
    planes[3] = r3 - r1;  // top
    planes[4] = r3 + r2;  // near
    planes[5] = r3 - r2;  // far
}

// Returns true if the patch's bounding sphere is entirely outside the frustum.
bool patchCulled(vec4 planes[6]) {
    // Patch world-space XZ center: inXZ in [-0.5, 0.5], world = xz * terrainSize
    vec2  xzAvg = (inXZ[0] + inXZ[1] + inXZ[2] + inXZ[3]) * 0.25;
    float patchWidth = pc.terrainSize / 32.0;  // TERRAIN_GRID_N = 32

    // Conservative sphere: center at half height, radius covers full patch diagonal
    vec3  center = vec3(xzAvg.x * pc.terrainSize,
                        pc.heightScale * 0.5,
                        xzAvg.y * pc.terrainSize);
    float radius = length(vec3(patchWidth * 0.5, pc.heightScale * 0.5, patchWidth * 0.5)) * 1.5;

    for (int i = 0; i < 6; i++) {
        // Signed distance of sphere center to plane (unnormalized → divide by |n|)
        float dist = dot(planes[i].xyz, center) + planes[i].w;
        float len  = length(planes[i].xyz);
        if (dist < -radius * len) return true;  // sphere fully on negative side
    }
    return false;
}

// ─── Distance-based tessellation level ───────────────────────────────────────
float tessLevel(float dist) {
    if (dist < 25.0)  return 64.0;
    if (dist < 100.0) return 32.0;
    if (dist < 250.0) return 8.0;
    return 2.0;
}

// Returns world-space midpoint of two control points on this patch.
vec3 edgeMid(int a, int b) {
    vec2 m = (inXZ[a] + inXZ[b]) * 0.5;
    return vec3(m.x * pc.terrainSize + pc.tileOffsetX,
                pc.heightScale * 0.3,
                m.y * pc.terrainSize + pc.tileOffsetZ);
}

void main() {
    outXZ[gl_InvocationID] = inXZ[gl_InvocationID];

    if (gl_InvocationID == 0) {
        vec4 planes[6];
        extractFrustumPlanes(pc.viewProj, planes);

        // ── Frustum cull ─────────────────────────────────────────────────────
        if (patchCulled(planes)) {
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelOuter[3] = 0.0;
            gl_TessLevelInner[0] = 0.0;
            gl_TessLevelInner[1] = 0.0;
            return;
        }

        // ── Per-edge tessellation levels (T-junction prevention) ──────────
        // Compute each outer level from the edge midpoint's distance to camera.
        // Control point order: [0]=BL, [1]=BR, [2]=TR, [3]=TL (CCW)
        // Tile offset applied so multi-tile edges match correctly.
        // TessOuter[0]=u=0/left, [1]=v=0/bottom, [2]=u=1/right, [3]=v=1/top

        float e0 = tessLevel(distance(pc.cameraPos, edgeMid(3, 0)));  // u=0 left  edge [TL-BL]
        float e1 = tessLevel(distance(pc.cameraPos, edgeMid(0, 1)));  // v=0 bottom edge [BL-BR]
        float e2 = tessLevel(distance(pc.cameraPos, edgeMid(1, 2)));  // u=1 right  edge [BR-TR]
        float e3 = tessLevel(distance(pc.cameraPos, edgeMid(2, 3)));  // v=1 top    edge [TR-TL]

        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelOuter[3] = e3;

        // Inner levels: based on patch center (average of outer levels keeps it smooth)
        vec2  xzAvg      = (inXZ[0] + inXZ[1] + inXZ[2] + inXZ[3]) * 0.25;
        vec3  patchCenter = vec3(xzAvg.x * pc.terrainSize + pc.tileOffsetX,
                                 pc.heightScale * 0.3,
                                 xzAvg.y * pc.terrainSize + pc.tileOffsetZ);
        float innerLevel  = tessLevel(distance(pc.cameraPos, patchCenter));
        gl_TessLevelInner[0] = innerLevel;
        gl_TessLevelInner[1] = innerLevel;
    }
}
