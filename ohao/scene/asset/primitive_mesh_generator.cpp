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

void PrimitiveMeshGenerator::generateSphere(Model& model, float radius, int segments) {
    model.vertices.clear();
    model.indices.clear();
    
    const float PI = glm::pi<float>();
    
    // Generate vertices
    for (int y = 0; y <= segments; ++y) {
        float v = (float)y / (float)segments;
        float phi = v * PI;
        
        for (int x = 0; x <= segments; ++x) {
            float u = (float)x / (float)segments;
            float theta = u * 2.0f * PI;
            
            float xPos = radius * std::sin(phi) * std::cos(theta);
            float yPos = radius * std::cos(phi);
            float zPos = radius * std::sin(phi) * std::sin(theta);
            
            glm::vec3 position(xPos, yPos, zPos);
            glm::vec3 normal = glm::normalize(position);
            glm::vec2 uv(u, v);
            glm::vec3 color(1.0f, 1.0f, 1.0f);
            
            model.vertices.push_back({ position, color, normal, uv });
        }
    }
    
    // Generate indices
    for (int y = 0; y < segments; ++y) {
        for (int x = 0; x < segments; ++x) {
            int current = y * (segments + 1) + x;
            int right = current + 1;
            int below = current + (segments + 1);
            int belowRight = below + 1;
            
            // Upper triangle
            model.indices.push_back(current);
            model.indices.push_back(below);
            model.indices.push_back(right);
            
            // Lower triangle
            model.indices.push_back(right);
            model.indices.push_back(below);
            model.indices.push_back(belowRight);
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