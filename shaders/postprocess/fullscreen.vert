#version 450

// Fullscreen Triangle Vertex Shader
// Generates a fullscreen triangle without any vertex input
// Uses vertex ID to generate positions: 0, 1, 2 -> triangle covering screen

layout(location = 0) out vec2 outTexCoord;

void main() {
    // Generate fullscreen triangle vertices
    // Vertex 0: (-1, -1) - bottom left
    // Vertex 1: (3, -1)  - far right (extends past screen)
    // Vertex 2: (-1, 3)  - far top (extends past screen)

    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );

    vec2 texCoords[3] = vec2[](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0)
    );

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    outTexCoord = texCoords[gl_VertexIndex];
}
