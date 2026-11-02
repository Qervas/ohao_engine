#pragma once

#include "model.hpp"

#include <memory>

namespace ohao {

class PrimitiveMeshGenerator {
public:
    static void generateCube(Model& model, float size = 1.0f);
    static void generateSphere(Model& model, float radius = 0.5f, int sectors = 32, int stacks = 16);
    static void generatePlatform(Model& model, float width = 2.0f, float height = 0.4f, float depth = 2.0f);
    static void generateCylinder(Model& model, float radius = 0.5f, float height = 1.0f, int segments = 16);
    static void generateCone(Model& model, float radius = 0.5f, float height = 1.0f, int segments = 16);
    // One quad a-b-c-d (two triangles 0-1-2, 0-2-3), every vertex with `normal` and
    // `color`, uv 0, tangent (1,0,0,1).
    static void generateQuad(Model& model, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 normal,
                             glm::vec3 color);
    // Axis-aligned box lo..hi: 6 faces x 4 vertices, flat normals, vertex color `color`.
    static void generateBox(Model& model, glm::vec3 lo, glm::vec3 hi, glm::vec3 color);

    /// Allocate + fill helpers (art-of-code factories).
    [[nodiscard]] static std::shared_ptr<Model> makeCube(float size = 1.0f) {
        auto m = std::make_shared<Model>();
        generateCube(*m, size);
        return m;
    }

    [[nodiscard]] static std::shared_ptr<Model> makeSphere(float radius = 0.5f, int sectors = 32, int stacks = 16) {
        auto m = std::make_shared<Model>();
        generateSphere(*m, radius, sectors, stacks);
        return m;
    }

    [[nodiscard]] static std::shared_ptr<Model> makePlatform(float width = 2.0f,
                                                            float height = 0.4f,
                                                            float depth = 2.0f) {
        auto m = std::make_shared<Model>();
        generatePlatform(*m, width, height, depth);
        return m;
    }

    [[nodiscard]] static std::shared_ptr<Model> makeCylinder(float radius = 0.5f,
                                                            float height = 1.0f,
                                                            int segments = 16) {
        auto m = std::make_shared<Model>();
        generateCylinder(*m, radius, height, segments);
        return m;
    }

    [[nodiscard]] static std::shared_ptr<Model> makeCone(float radius = 0.5f,
                                                        float height = 1.0f,
                                                        int segments = 16) {
        auto m = std::make_shared<Model>();
        generateCone(*m, radius, height, segments);
        return m;
    }
};

} // namespace ohao
