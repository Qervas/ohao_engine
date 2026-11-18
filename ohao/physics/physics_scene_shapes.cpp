// The physics scene's collider shapes (physics/physics_scene.hpp): a collider fitted to a mesh (fitCollider), the mesh it
// is fitted to, and the backend's shapes shared among bodies of one mesh.
#include "physics/private/physics_scene_impl.hpp"
#include "visual/components.hpp"

#include <algorithm>

namespace ohao::ecs {

FittedCollider fitCollider(const MeshData& mesh, ColliderShape shape, glm::vec3 scale) {
    FittedCollider f;
    f.shape = shape;
    // The mesh's own AABB: spheres and capsules are fitted to it, then scaled by the largest axis.
    glm::vec3 lo(0.0f), hi(0.0f);
    if (!mesh.positions.empty()) {
        lo = hi = mesh.positions.front();
        for (const glm::vec3& p : mesh.positions) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    const glm::vec3 centre = (lo + hi) * 0.5f, half = (hi - lo) * 0.5f;
    const float largest = maxAbs(scale);
    f.vertices.reserve(mesh.positions.size());
    for (const glm::vec3& p : mesh.positions) f.vertices.push_back(p * scale);
    switch (shape) {
        case ColliderShape::Box:
            f.halfExtents = half * glm::abs(scale);
            f.offset = centre * scale;
            break;
        case ColliderShape::Sphere: {
            float r2 = 0.0f;
            for (const glm::vec3& p : mesh.positions) r2 = std::max(r2, glm::dot(p - centre, p - centre));
            f.radius = std::sqrt(r2) * largest;
            f.offset = centre * scale;
            f.roundedScale = nonUniform(scale);
            break;
        }
        case ColliderShape::Capsule: {
            const int axis = half.x >= half.y && half.x >= half.z ? 0 : half.y >= half.z ? 1 : 2;
            const float a = half[(axis + 1) % 3], b = half[(axis + 2) % 3];
            f.radius = std::max(a, b) * largest;
            f.halfHeight = std::max(half[axis] * largest - f.radius, 0.0f);
            // The backend's capsule runs along +Y.
            if (axis == 0) f.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            if (axis == 2) f.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            f.offset = centre * scale;
            f.roundedScale = nonUniform(scale);
            break;
        }
        case ColliderShape::Mesh:
            f.indices = mesh.indices;
            // A mirror turns the triangles inside out; swapping two corners turns them back.
            if (scale.x * scale.y * scale.z < 0.0f)
                for (std::size_t i = 0; i + 2 < f.indices.size(); i += 3) std::swap(f.indices[i + 1], f.indices[i + 2]);
            break;
        case ColliderShape::Convex:
        case ColliderShape::None:
            break;
    }
    if (shape != ColliderShape::Convex && shape != ColliderShape::Mesh) f.vertices.clear();
    return f;
}

// The mesh a collider is fitted to: a CollisionMesh, else the entity's MeshSource.
auto PhysicsScene::Impl::meshOf(entt::entity e) -> std::shared_ptr<const cooked::Mesh> {
    const MeshSource* source = nullptr;
    MeshSource fromCollision;
    if (const auto* cm = reg.try_get<CollisionMesh>(e)) {
        fromCollision.source = cm->source;
        source = &fromCollision;
    } else {
        source = reg.try_get<MeshSource>(e);
    }
    if (!source) return nullptr;
    if (const auto* asset = std::get_if<ModelAsset>(&source->source)) {
        auto loaded = assets.loadMesh(asset->asset, asset->mesh);
        if (!loaded) {
            warn(e, "W_COLLIDER_NO_MESH", "its mesh did not load (" + loaded.error().message + "); no collider");
            return nullptr;
        }
        return std::move(loaded.value());
    }
    return primitiveMesh(*source);
}

// One collider as a backend shape in the body's frame (scale baked), or nullopt.
auto PhysicsScene::Impl::sharedShape(const cooked::Mesh& mesh, ColliderShape shape) -> std::shared_ptr<const pb::SharedShape> {
    if (!mesh.id || mesh.vertices.empty()) return nullptr;
    auto& slot = sharedShapes[{mesh.id, shape}];
    if (auto made = slot.lock()) return made;
    pb::IPhysicsBackend& be = ensureBackend();
    // The cooked shape's name: the mesh, which shape of it, and whose bytes they are.
    ContentHasher name;
    name.value(mesh.id.hi);
    name.value(mesh.id.lo);
    name.value(static_cast<uint32_t>(shape));
    name.value(be.sharedFormat());
    const std::filesystem::path file = assets.shapeFile(name.finish());
    std::shared_ptr<const pb::SharedShape> made;
    if (!file.empty())
        if (const auto cooked = cooked::Mapping::open(file)) made = be.loadShared(cooked.value()->bytes());
    if (!made) {
        std::vector<glm::vec3> positions;
        positions.reserve(mesh.vertices.size());
        for (const Vertex& v : mesh.vertices) positions.push_back(v.position);
        pb::ShapeInfo info;
        if (shape == ColliderShape::Mesh) {
            info.setMesh(positions, mesh.indices);
        } else {
            info.type = pb::ShapeInfo::Type::CONVEX_HULL;
            info.meshVertices = positions.data();
            info.meshVertexCount = static_cast<uint32_t>(positions.size());
        }
        made = be.makeShared(info);
        if (made && !file.empty())
            if (const auto written = cooked::writeFile(file, be.saveShared(*made)); !written)
                OHAO_LOG(Warn, physics, "a collision shape was not kept: {}", written.error());
    }
    slot = made;
    return made;
}

auto PhysicsScene::Impl::shapeOf(entt::entity e, const Collider& c, glm::vec3 scale, Motion motion, std::vector<std::shared_ptr<FittedCollider>>& keep, BodyRecord::Kind& kind, std::vector<std::shared_ptr<const pb::SharedShape>>& held) -> std::optional<pb::ShapeInfo> {
    ColliderShape shape = c.shape;
    if (shape == ColliderShape::Mesh && motion == Motion::Dynamic) {
        warn(e, "W_LEVEL_DYNAMIC_MESH", "a dynamic body cannot have a triangle-mesh collider; it is built as the mesh's convex hull");
        shape = ColliderShape::Convex;
    }
    auto fitted = std::make_shared<FittedCollider>();
    const bool explicitSize = c.size != glm::vec3(0.0f) &&
                              (shape == ColliderShape::Box || shape == ColliderShape::Sphere || shape == ColliderShape::Capsule);
    if (explicitSize) {
        fitted->shape = shape;
        fitted->offset = c.offset * scale;
        fitted->halfExtents = c.size * glm::abs(scale);
        fitted->radius = c.size.x * maxAbs(scale);
        fitted->halfHeight = c.size.y * maxAbs(scale);
        fitted->roundedScale = shape != ColliderShape::Box && nonUniform(scale);
    } else {
        const auto model = meshOf(e);
        if (!model || model->vertices.empty()) {
            warn(e, "W_COLLIDER_NO_MESH", "its collider has no size and the entity no mesh to fit it to; no collider");
            return std::nullopt;
        }
        // A mesh or a hull of a cooked mesh: the mesh's one shape, at this body's scale.
        if (shape == ColliderShape::Mesh || shape == ColliderShape::Convex) {
            if (auto shared = sharedShape(*model, shape)) {
                pb::ShapeInfo s;
                s.type = pb::ShapeInfo::Type::SHARED;
                s.shared = shared;
                s.sharedScale = scale;
                kind = shape == ColliderShape::Mesh ? BodyRecord::Kind::Mesh : BodyRecord::Kind::Convex;
                held.push_back(std::move(shared));
                return s;
            }
        }
        MeshData data;
        data.positions.reserve(model->vertices.size());
        for (const auto& v : model->vertices) data.positions.push_back(v.position);
        if (shape == ColliderShape::Mesh) data.indices.assign(model->indices.begin(), model->indices.end());
        *fitted = fitCollider(data, shape, scale);
    }
    if (fitted->roundedScale)
        warn(e, "W_COLLIDER_SCALE", "a sphere or capsule cannot stretch unevenly; it takes the largest scale axis");

    pb::ShapeInfo s;
    bool placed = false;   // the shape sits away from the body's origin (or turned)
    switch (shape) {
        case ColliderShape::Box:
            s.type = pb::ShapeInfo::Type::BOX;
            s.halfExtents = glm::max(fitted->halfExtents, glm::vec3(0.001f));
            kind = BodyRecord::Kind::Box;
            placed = fitted->offset != glm::vec3(0.0f);
            break;
        case ColliderShape::Sphere:
            s.type = pb::ShapeInfo::Type::SPHERE;
            s.radius = std::max(fitted->radius, 0.001f);
            kind = BodyRecord::Kind::Sphere;
            placed = fitted->offset != glm::vec3(0.0f);
            break;
        case ColliderShape::Capsule:
            s.type = pb::ShapeInfo::Type::CAPSULE;
            s.radius = std::max(fitted->radius, 0.001f);
            s.height = 2.0f * (fitted->halfHeight + s.radius);   // the backend's height is end to end
            kind = BodyRecord::Kind::Capsule;
            placed = fitted->offset != glm::vec3(0.0f) || fitted->rotation != glm::quat(1, 0, 0, 0);
            break;
        case ColliderShape::Convex:
            s.type = pb::ShapeInfo::Type::CONVEX_HULL;
            s.meshVertices = fitted->vertices.data();
            s.meshVertexCount = static_cast<uint32_t>(fitted->vertices.size());
            kind = BodyRecord::Kind::Convex;
            break;
        case ColliderShape::Mesh:
            s.setMesh(fitted->vertices, fitted->indices);
            kind = BodyRecord::Kind::Mesh;
            break;
        case ColliderShape::None:
            return std::nullopt;
    }
    keep.push_back(fitted);
    if (!placed) return s;
    pb::ShapeInfo wrap;
    wrap.type = pb::ShapeInfo::Type::COMPOUND;
    wrap.children.push_back({std::move(s), fitted->offset, fitted->rotation});
    return wrap;
}

} // namespace ohao::ecs
