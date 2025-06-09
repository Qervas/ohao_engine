#pragma once

#include "model.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace ohao {

class PrimitiveMeshGenerator {
public:
    static void generateCube(Model& model, float size = 1.0f);
    static void generateSphere(Model& model, float radius = 0.5f, int segments = 16);
    static void generatePlane(Model& model, float width = 1.0f, float height = 1.0f);
    static void generateCylinder(Model& model, float radius = 0.5f, float height = 1.0f, int segments = 16);
    static void generateCone(Model& model, float radius = 0.5f, float height = 1.0f, int segments = 16);
};

} // namespace ohao 