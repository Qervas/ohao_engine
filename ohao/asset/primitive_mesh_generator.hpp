#pragma once

#include "asset/vertex.hpp"

#include <cstdint>
#include <vector>

namespace ohao {

// A mesh made in code.
struct PrimitiveMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

class PrimitiveMeshGenerator {
public:
    static void generateCube(PrimitiveMesh& model, float size = 1.0f);
    static void generateSphere(PrimitiveMesh& model, float radius = 0.5f, int sectors = 32, int stacks = 16);
    static void generatePlatform(PrimitiveMesh& model, float width = 2.0f, float height = 0.4f, float depth = 2.0f);
    static void generateCylinder(PrimitiveMesh& model, float radius = 0.5f, float height = 1.0f, int segments = 16);
    static void generateCone(PrimitiveMesh& model, float radius = 0.5f, float height = 1.0f, int segments = 16);
    // One quad a-b-c-d (two triangles 0-1-2, 0-2-3), every vertex with `normal` and
    // `color`, uv 0, tangent (1,0,0,1).
    static void generateQuad(PrimitiveMesh& model, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 normal,
                             glm::vec3 color);
    // Axis-aligned box lo..hi: 6 faces x 4 vertices, flat normals, vertex color `color`.
    static void generateBox(PrimitiveMesh& model, glm::vec3 lo, glm::vec3 hi, glm::vec3 color);
};

} // namespace ohao
