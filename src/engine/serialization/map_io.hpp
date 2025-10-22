#pragma once
#include <string>
#include <nlohmann/json_fwd.hpp>

namespace ohao {

class Scene;
class Actor;
class TransformComponent;
class MeshComponent;
class PhysicsComponent;

// AAA-style Map IO (versioned JSON map with actors/components)
class MapIO {
public:
    explicit MapIO(Scene* scene);

    bool save(const std::string& filePath);
    bool load(const std::string& filePath);

    // Version helpers
    static const char* kMagic();
    static int kVersion();

private:
    Scene* scene;

    // Actor helpers
    nlohmann::json serializeActor(const Actor* actor) const;
    bool deserializeActor(const nlohmann::json& j);

    // Component helpers
    static nlohmann::json serializeTransform(const TransformComponent* tc);
    static void deserializeTransform(TransformComponent* tc, const nlohmann::json& j);

    static nlohmann::json serializeMesh(const MeshComponent* mc);
    static void deserializeMesh(MeshComponent* mc, const nlohmann::json& j);

    static nlohmann::json serializePhysics(const PhysicsComponent* pc);
    static void deserializePhysics(PhysicsComponent* pc, const nlohmann::json& j);
};

} // namespace ohao


