#pragma once

#include "model.hpp"

#include <memory>

namespace ohao {

class PrimitiveMeshGenerator {
public:
    static void generateCube(Model& model, float size = 1.0f);
    static void generateSphere(Model& model, float radius = 0.5f, int segments = 16);
    static void generatePlatform(Model& model, float width = 2.0f, float height = 0.4f, float depth = 2.0f);
    static void generateCylinder(Model& model, float radius = 0.5f, float height = 1.0f, int segments = 16);
    static void generateCone(Model& model, float radius = 0.5f, float height = 1.0f, int segments = 16);

    /// Allocate + fill helpers (art-of-code factories).
    [[nodiscard]] static std::shared_ptr<Model> makeCube(float size = 1.0f) {
        auto m = std::make_shared<Model>();
        generateCube(*m, size);
        return m;
    }

    [[nodiscard]] static std::shared_ptr<Model> makeSphere(float radius = 0.5f, int segments = 16) {
        auto m = std::make_shared<Model>();
        generateSphere(*m, radius, segments);
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
