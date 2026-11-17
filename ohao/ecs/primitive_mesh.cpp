#include "ecs/primitive_mesh.hpp"

#include "asset/primitive_mesh_generator.hpp"

#include <variant>

namespace ohao::ecs {

namespace {
template <class... F>
struct Overloaded : F... { using F::operator()...; };
template <class... F>
Overloaded(F...) -> Overloaded<F...>;
} // namespace

std::shared_ptr<const cooked::Mesh> primitiveMesh(const MeshSource& ms) {
    if (std::holds_alternative<ModelAsset>(ms.source)) return nullptr;
    struct Made {
        PrimitiveMesh data;
        cooked::Mesh view;
    };
    auto made = std::make_shared<Made>();
    PrimitiveMesh* m = &made->data;
    std::visit(Overloaded{
                   [](const ModelAsset&) {},
                   [&](const SpherePrim& p) { PrimitiveMeshGenerator::generateSphere(*m, p.radius, p.sectors, p.stacks); },
                   [&](const CubePrim& p) { PrimitiveMeshGenerator::generateCube(*m, p.size); },
                   [&](const PlatformPrim& p) {
                       PrimitiveMeshGenerator::generatePlatform(*m, p.width, p.height, p.depth);
                   },
                   [&](const CylinderPrim& p) {
                       PrimitiveMeshGenerator::generateCylinder(*m, p.radius, p.height, p.segments);
                   },
                   [&](const ConePrim& p) { PrimitiveMeshGenerator::generateCone(*m, p.radius, p.height, p.segments); },
                   [&](const QuadPrim& p) { PrimitiveMeshGenerator::generateQuad(*m, p.a, p.b, p.c, p.d, p.normal, p.color); },
                   [&](const BoxPrim& p) { PrimitiveMeshGenerator::generateBox(*m, p.lo, p.hi, p.color); },
               },
               ms.source);
    made->view.vertices = made->data.vertices;
    made->view.indices = made->data.indices;
    return {made, &made->view};
}

} // namespace ohao::ecs
