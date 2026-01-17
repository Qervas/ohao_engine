#version 450

// Cascaded Shadow Map Geometry Shader
// Broadcasts each triangle to all cascade layers in a single pass

layout(triangles) in;
layout(triangle_strip, max_vertices = 12) out; // 4 cascades * 3 vertices

layout(location = 0) in vec3 inWorldPos[];

#define NUM_CASCADES 4

// Cascade view-projection matrices
layout(set = 0, binding = 0) uniform CascadeMatrices {
    mat4 viewProj[NUM_CASCADES];
} cascades;

void main() {
    // Emit triangle to each cascade
    for (int cascade = 0; cascade < NUM_CASCADES; ++cascade) {
        gl_Layer = cascade;  // Select cascade layer in texture array

        for (int i = 0; i < 3; ++i) {
            vec4 worldPos = vec4(inWorldPos[i], 1.0);
            gl_Position = cascades.viewProj[cascade] * worldPos;
            EmitVertex();
        }

        EndPrimitive();
    }
}
