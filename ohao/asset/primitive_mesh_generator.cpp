#include "primitive_mesh_generator.hpp"
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace ohao {

void PrimitiveMeshGenerator::generateCube(Model& model, float size) {
    model.vertices.clear();
    model.indices.clear();
    
    float halfSize = size * 0.5f;
    
    // Define the 8 vertices of the cube
    std::vector<glm::vec3> positions = {
        // Front face
        { -halfSize, -halfSize,  halfSize }, // 0
        {  halfSize, -halfSize,  halfSize }, // 1
        {  halfSize,  halfSize,  halfSize }, // 2
        { -halfSize,  halfSize,  halfSize }, // 3
        
        // Back face
        { -halfSize, -halfSize, -halfSize }, // 4
        {  halfSize, -halfSize, -halfSize }, // 5
        {  halfSize,  halfSize, -halfSize }, // 6
        { -halfSize,  halfSize, -halfSize }  // 7
    };
    
    // Define normals for each face
    std::vector<glm::vec3> normals = {
        { 0.0f, 0.0f, 1.0f },  // Front
        { 0.0f, 0.0f, -1.0f }, // Back
        { 1.0f, 0.0f, 0.0f },  // Right
        { -1.0f, 0.0f, 0.0f }, // Left
        { 0.0f, 1.0f, 0.0f },  // Top
        { 0.0f, -1.0f, 0.0f }  // Bottom
    };
    
    // Define colors (just use white for all vertices)
    glm::vec3 color(1.0f, 1.0f, 1.0f);
    
    // Define UVs for each face
    std::vector<glm::vec2> uvs = {
        { 0.0f, 0.0f }, // Bottom-left
        { 1.0f, 0.0f }, // Bottom-right
        { 1.0f, 1.0f }, // Top-right
        { 0.0f, 1.0f }  // Top-left
    };
    
    // Add the vertices for each face
    // Front face
    model.vertices.push_back({ positions[0], color, normals[0], uvs[0] });
    model.vertices.push_back({ positions[1], color, normals[0], uvs[1] });
    model.vertices.push_back({ positions[2], color, normals[0], uvs[2] });
    model.vertices.push_back({ positions[3], color, normals[0], uvs[3] });
    
    // Back face
    model.vertices.push_back({ positions[5], color, normals[1], uvs[0] });
    model.vertices.push_back({ positions[4], color, normals[1], uvs[1] });
    model.vertices.push_back({ positions[7], color, normals[1], uvs[2] });
    model.vertices.push_back({ positions[6], color, normals[1], uvs[3] });
    
    // Right face
    model.vertices.push_back({ positions[1], color, normals[2], uvs[0] });
    model.vertices.push_back({ positions[5], color, normals[2], uvs[1] });
    model.vertices.push_back({ positions[6], color, normals[2], uvs[2] });
    model.vertices.push_back({ positions[2], color, normals[2], uvs[3] });
    
    // Left face
    model.vertices.push_back({ positions[4], color, normals[3], uvs[0] });
    model.vertices.push_back({ positions[0], color, normals[3], uvs[1] });
    model.vertices.push_back({ positions[3], color, normals[3], uvs[2] });
    model.vertices.push_back({ positions[7], color, normals[3], uvs[3] });
    
    // Top face
    model.vertices.push_back({ positions[3], color, normals[4], uvs[0] });
    model.vertices.push_back({ positions[2], color, normals[4], uvs[1] });
    model.vertices.push_back({ positions[6], color, normals[4], uvs[2] });
    model.vertices.push_back({ positions[7], color, normals[4], uvs[3] });
    
    // Bottom face
    model.vertices.push_back({ positions[4], color, normals[5], uvs[0] });
    model.vertices.push_back({ positions[5], color, normals[5], uvs[1] });
    model.vertices.push_back({ positions[1], color, normals[5], uvs[2] });
    model.vertices.push_back({ positions[0], color, normals[5], uvs[3] });
    
    // Add indices for each face (6 faces * 2 triangles * 3 indices = 36 indices)
    for (uint32_t i = 0; i < 6; ++i) {
        uint32_t baseIndex = i * 4;
        
        // First triangle
        model.indices.push_back(baseIndex + 0);
        model.indices.push_back(baseIndex + 1);
        model.indices.push_back(baseIndex + 2);
        
        // Second triangle
        model.indices.push_back(baseIndex + 0);
        model.indices.push_back(baseIndex + 2);
        model.indices.push_back(baseIndex + 3);
    }
}

// UV sphere: stacks latitude bands x sectors longitude segments, normal =
// normalized position. The defaults (32 x 16) are the sphere the examples render.
void PrimitiveMeshGenerator::generateSphere(Model& model, float radius, int sectors, int stacks) {
    model.vertices.clear();
    model.indices.clear();

    for (int i = 0; i <= stacks; ++i) {
        const float phi = glm::pi<float>() * float(i) / float(stacks);
        const float sinPhi = std::sin(phi);
        const float cosPhi = std::cos(phi);
        for (int j = 0; j <= sectors; ++j) {
            const float theta = 2.0f * glm::pi<float>() * float(j) / float(sectors);
            const float x = std::cos(theta) * sinPhi;
            const float y = cosPhi;
            const float z = std::sin(theta) * sinPhi;
            Vertex vertex;
            vertex.position = {x * radius, y * radius, z * radius};
            vertex.normal = {x, y, z};
            vertex.color = {1.0f, 1.0f, 1.0f};
            vertex.texCoord = {float(j) / sectors, float(i) / stacks};
            // Along the parallel (increasing theta): what an anisotropic material stretches along.
            vertex.tangent = {-std::sin(theta), 0.0f, std::cos(theta), 1.0f};
            model.vertices.push_back(vertex);
        }
    }

    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < sectors; ++j) {
            const int first = i * (sectors + 1) + j;
            const int second = first + sectors + 1;
            model.indices.push_back(first);
            model.indices.push_back(second);
            model.indices.push_back(first + 1);
            model.indices.push_back(second);
            model.indices.push_back(second + 1);
            model.indices.push_back(first + 1);
        }
    }
}

void PrimitiveMeshGenerator::generatePlatform(Model& model, float width, float height, float depth) {
    model.vertices.clear();
    model.indices.clear();
    
    // Generate a thick platform (essentially a flattened box)
    float halfWidth = width * 0.5f;
    float halfHeight = height * 0.5f;
    float halfDepth = depth * 0.5f;

    // Define the 8 corners of the platform box
    glm::vec3 corners[8] = {
        {-halfWidth, -halfHeight, -halfDepth}, // 0: bottom-left-back
        { halfWidth, -halfHeight, -halfDepth}, // 1: bottom-right-back
        { halfWidth, -halfHeight,  halfDepth}, // 2: bottom-right-front
        {-halfWidth, -halfHeight,  halfDepth}, // 3: bottom-left-front
        {-halfWidth,  halfHeight, -halfDepth}, // 4: top-left-back
        { halfWidth,  halfHeight, -halfDepth}, // 5: top-right-back
        { halfWidth,  halfHeight,  halfDepth}, // 6: top-right-front
        {-halfWidth,  halfHeight,  halfDepth}  // 7: top-left-front
    };

    // Face normals
    glm::vec3 normals[6] = {
        { 0.0f,  1.0f,  0.0f}, // top
        { 0.0f, -1.0f,  0.0f}, // bottom
        { 0.0f,  0.0f,  1.0f}, // front
        { 0.0f,  0.0f, -1.0f}, // back
        { 1.0f,  0.0f,  0.0f}, // right
        {-1.0f,  0.0f,  0.0f}  // left
    };

    // Define faces (each face has 4 vertices)
    int faceVertices[6][4] = {
        {7, 6, 5, 4}, // top face
        {0, 1, 2, 3}, // bottom face
        {3, 2, 6, 7}, // front face
        {4, 5, 1, 0}, // back face
        {2, 1, 5, 6}, // right face
        {0, 3, 7, 4}  // left face
    };

    // UV coordinates for each face
    glm::vec2 faceUVs[4] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
    };

    // Generate vertices for each face
    for (int face = 0; face < 6; ++face) {
        for (int vert = 0; vert < 4; ++vert) {
            Vertex vertex;
            vertex.position = corners[faceVertices[face][vert]];
            vertex.normal = normals[face];
            vertex.color = glm::vec3(1.0f); // White color
            vertex.texCoord = faceUVs[vert];
            model.vertices.push_back(vertex);
        }
    }

    // Generate indices (2 triangles per face)
    for (int face = 0; face < 6; ++face) {
        int baseIndex = face * 4;
        
        // First triangle
        model.indices.push_back(baseIndex + 0);
        model.indices.push_back(baseIndex + 1);
        model.indices.push_back(baseIndex + 2);
        
        // Second triangle
        model.indices.push_back(baseIndex + 2);
        model.indices.push_back(baseIndex + 3);
        model.indices.push_back(baseIndex + 0);
    }
}

void PrimitiveMeshGenerator::generateCylinder(Model& model, float radius, float height, int segments) {
    model.vertices.clear();
    model.indices.clear();
    
    const float PI = glm::pi<float>();
    float halfHeight = height * 0.5f;
    
    // Generate vertices for the cylinder
    glm::vec3 color(1.0f, 1.0f, 1.0f);
    
    // Top center vertex
    model.vertices.push_back({ { 0.0f, halfHeight, 0.0f }, color, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f } });
    
    // Bottom center vertex
    model.vertices.push_back({ { 0.0f, -halfHeight, 0.0f }, color, { 0.0f, -1.0f, 0.0f }, { 0.5f, 0.5f } });
    
    // Generate vertices for the top, bottom, and sides
    for (int i = 0; i <= segments; ++i) {
        float angle = 2.0f * PI * (float)i / (float)segments;
        float x = radius * std::cos(angle);
        float z = radius * std::sin(angle);
        float u = (std::cos(angle) + 1.0f) * 0.5f;
        float v = (std::sin(angle) + 1.0f) * 0.5f;
        
        // Top rim vertex
        model.vertices.push_back({ { x, halfHeight, z }, color, { 0.0f, 1.0f, 0.0f }, { u, v } });
        
        // Bottom rim vertex
        model.vertices.push_back({ { x, -halfHeight, z }, color, { 0.0f, -1.0f, 0.0f }, { u, v } });
        
        // Side vertex (top)
        model.vertices.push_back({ { x, halfHeight, z }, color, glm::normalize(glm::vec3(x, 0.0f, z)), { (float)i / segments, 1.0f } });
        
        // Side vertex (bottom)
        model.vertices.push_back({ { x, -halfHeight, z }, color, glm::normalize(glm::vec3(x, 0.0f, z)), { (float)i / segments, 0.0f } });
    }
    
    // Add indices for top cap
    for (int i = 0; i < segments; ++i) {
        model.indices.push_back(0); // Top center
        model.indices.push_back(2 + i * 4);
        model.indices.push_back(2 + ((i + 1) % segments) * 4);
    }
    
    // Add indices for bottom cap
    for (int i = 0; i < segments; ++i) {
        model.indices.push_back(1); // Bottom center
        model.indices.push_back(3 + ((i + 1) % segments) * 4);
        model.indices.push_back(3 + i * 4);
    }
    
    // Add indices for sides
    for (int i = 0; i < segments; ++i) {
        int current = 4 + i * 4;
        int next = 4 + ((i + 1) % segments) * 4;
        
        // First triangle
        model.indices.push_back(current);
        model.indices.push_back(current + 1);
        model.indices.push_back(next);
        
        // Second triangle
        model.indices.push_back(next);
        model.indices.push_back(current + 1);
        model.indices.push_back(next + 1);
    }
}

// Moved verbatim from the examples' quad builders (cornell_box addQuad, model_viewer
// addQuad), so scenes built from QuadPrim keep byte-identical vertex streams.
void PrimitiveMeshGenerator::generateQuad(Model& model, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                                          glm::vec3 normal, glm::vec3 color) {
    model.vertices.clear();
    model.indices.clear();
    auto makeVert = [&](glm::vec3 pos) {
        Vertex v{};
        v.position = pos;
        v.normal = normal;
        v.color = color;
        v.texCoord = glm::vec2(0);
        v.tangent = glm::vec4(1,0,0,1);
        v.boneIndices = glm::ivec4(0);
        v.boneWeights = glm::vec4(1,0,0,0);
        return v;
    };
    model.vertices = {makeVert(a), makeVert(b), makeVert(c), makeVert(d)};
    model.indices = {0, 1, 2, 0, 2, 3};
}

// Moved verbatim from model_viewer's addBox: 6 faces of 4 vertices, flat normals.
void PrimitiveMeshGenerator::generateBox(Model& model, glm::vec3 lo, glm::vec3 hi, glm::vec3 color) {
    model.vertices.clear();
    model.indices.clear();
    auto mkv = [&](glm::vec3 pos, glm::vec3 n) {
        Vertex v{}; v.position = pos; v.normal = n; v.color = color;
        v.texCoord = {0,0}; v.tangent = {1,0,0,1};
        v.boneIndices = glm::ivec4(0); v.boneWeights = {1,0,0,0};
        return v;
    };
    glm::vec3 c[8] = {
        {lo.x,lo.y,lo.z}, {hi.x,lo.y,lo.z}, {hi.x,hi.y,lo.z}, {lo.x,hi.y,lo.z},
        {lo.x,lo.y,hi.z}, {hi.x,lo.y,hi.z}, {hi.x,hi.y,hi.z}, {lo.x,hi.y,hi.z}
    };
    // 6 faces: front, back, left, right, top, bottom
    int faces[6][4] = {{4,5,6,7},{1,0,3,2},{0,4,7,3},{5,1,2,6},{3,7,6,2},{0,1,5,4}};
    glm::vec3 normals[6] = {{0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    for (int f = 0; f < 6; f++) {
        uint32_t base = (uint32_t)model.vertices.size();
        for (int k = 0; k < 4; k++) model.vertices.push_back(mkv(c[faces[f][k]], normals[f]));
        model.indices.insert(model.indices.end(), {base,base+1,base+2, base,base+2,base+3});
    }
}

void PrimitiveMeshGenerator::generateCone(Model& model, float radius, float height, int segments) {
    model.vertices.clear();
    model.indices.clear();
    
    const float PI = glm::pi<float>();
    float halfHeight = height * 0.5f;
    glm::vec3 color(1.0f, 1.0f, 1.0f);
    
    // Top vertex (apex)
    model.vertices.push_back({ { 0.0f, halfHeight, 0.0f }, color, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f } });
    
    // Bottom center vertex
    model.vertices.push_back({ { 0.0f, -halfHeight, 0.0f }, color, { 0.0f, -1.0f, 0.0f }, { 0.5f, 0.5f } });
    
    // Generate vertices for the bottom and sides
    for (int i = 0; i <= segments; ++i) {
        float angle = 2.0f * PI * (float)i / (float)segments;
        float x = radius * std::cos(angle);
        float z = radius * std::sin(angle);
        float u = (std::cos(angle) + 1.0f) * 0.5f;
        float v = (std::sin(angle) + 1.0f) * 0.5f;
        
        // Bottom rim vertex
        model.vertices.push_back({ { x, -halfHeight, z }, color, { 0.0f, -1.0f, 0.0f }, { u, v } });
        
        // Calculate the normal for the side
        glm::vec3 normal = glm::normalize(glm::vec3(x, radius / height, z));
        
        // Side vertex
        model.vertices.push_back({ { x, -halfHeight, z }, color, normal, { (float)i / segments, 0.0f } });
    }
    
    // Add indices for bottom cap
    for (int i = 0; i < segments; ++i) {
        model.indices.push_back(1); // Bottom center
        model.indices.push_back(2 + ((i + 1) % segments) * 2);
        model.indices.push_back(2 + i * 2);
    }
    
    // Add indices for sides
    for (int i = 0; i < segments; ++i) {
        model.indices.push_back(0); // Top vertex
        model.indices.push_back(3 + i * 2);
        model.indices.push_back(3 + ((i + 1) % segments) * 2);
    }
}

} // namespace ohao 