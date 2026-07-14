#pragma once

/**
 * Scene — owns actors, mesh/physics registries, and the physics world.
 *
 * Art notes (core style template applied):
 *   - string_view at query boundaries; owned strings in maps
 *   - [[nodiscard]] on lookups / fallible ops
 *   - ranges-friendly forEach* helpers for non-hot iteration
 *   - actor tags for findActorsByTag
 */

#include "scene/actor/actor.hpp"
#include "scene/component/component_factory.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace ohao {
namespace physics {
class PhysicsWorld;
}

class MeshComponent;
class PhysicsComponent;

struct SceneDescriptor {
    std::string name;
    std::string version{"1.0"};
    std::vector<std::string> tags;
    std::string createdBy;
    std::string lastModified;
    std::unordered_map<std::string, std::string> metadata;
};

class Scene {
public:
    using Ptr = std::shared_ptr<Scene>;
    using ActorMap = std::unordered_map<std::uint64_t, Actor::Ptr>;
    using ActorNameMap = std::unordered_map<std::string, Actor::Ptr>;

    explicit Scene(std::string_view name = "New Scene");
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // ─── Actor management ───────────────────────────────────────────────────
    [[nodiscard]] Actor::Ptr createActor(std::string_view name = "Actor");
    [[nodiscard]] Actor::Ptr createActorWithComponents(std::string_view name, PrimitiveType primitiveType);

    void addActor(Actor::Ptr actor);
    void removeActor(Actor::Ptr actor);
    void removeActor(std::string_view name);
    void removeActor(std::uint64_t id);
    void removeAllActors();

    // ─── Lookup ─────────────────────────────────────────────────────────────
    [[nodiscard]] Actor::Ptr findActor(std::string_view name) const;
    [[nodiscard]] Actor::Ptr findActor(std::uint64_t id) const;
    [[nodiscard]] std::vector<Actor::Ptr> findActorsByName(std::string_view partialName) const;
    [[nodiscard]] std::vector<Actor::Ptr> findActorsByTag(std::string_view tag) const;
    [[nodiscard]] const ActorMap& getAllActors() const noexcept { return actors; }

    [[nodiscard]] std::size_t actorCount() const noexcept { return actors.size(); }
    [[nodiscard]] bool empty() const noexcept { return actors.empty(); }
    [[nodiscard]] bool contains(std::string_view name) const { return findActor(name) != nullptr; }
    [[nodiscard]] bool contains(std::uint64_t id) const { return findActor(id) != nullptr; }

    /// Visit every actor (including inactive). Non-hot-path convenience.
    template<typename F>
        requires std::invocable<F&, Actor&>
    void forEachActor(F&& fn) {
        for (auto& [id, actor] : actors) {
            if (actor) fn(*actor);
        }
    }

    template<typename F>
        requires std::invocable<F&, const Actor&>
    void forEachActor(F&& fn) const {
        for (const auto& [id, actor] : actors) {
            if (actor) fn(*actor);
        }
    }

    // Convenience aliases (legacy)
    void addObject(std::string_view name, Actor::Ptr actor) {
        actorsByName[std::string(name)] = actor;
        actors[actor->getID()] = actor;
    }
    void removeObject(std::string_view name) { removeActor(name); }
    [[nodiscard]] Actor::Ptr getObjectByID(std::uint64_t id) { return findActor(id); }
    [[nodiscard]] const ActorNameMap& getObjectsByName() const noexcept { return actorsByName; }

    // ─── Component notifications ────────────────────────────────────────────
    void onMeshComponentAdded(MeshComponent* component);
    void onMeshComponentRemoved(MeshComponent* component);
    void onMeshComponentChanged(MeshComponent* component);

    void onPhysicsComponentAdded(PhysicsComponent* component);
    void onPhysicsComponentRemoved(PhysicsComponent* component);

    // ─── Physics ────────────────────────────────────────────────────────────
    void updatePhysics(float deltaTime);
    [[nodiscard]] physics::PhysicsWorld* getPhysicsWorld() noexcept { return physicsWorld.get(); }
    [[nodiscard]] const physics::PhysicsWorld* getPhysicsWorld() const noexcept { return physicsWorld.get(); }
    [[nodiscard]] const std::vector<PhysicsComponent*>& getPhysicsComponents() const noexcept {
        return physicsComponents;
    }
    [[nodiscard]] const std::vector<MeshComponent*>& getMeshComponents() const noexcept {
        return meshComponents;
    }

    void addPhysicsToAllObjects();

    // ─── Properties ─────────────────────────────────────────────────────────
    [[nodiscard]] const std::string& getName() const noexcept;
    void setName(std::string_view name);

    void initialize();
    void update(float deltaTime);
    void render();
    void destroy();

    [[nodiscard]] bool importModel(std::string_view filename, Actor::Ptr targetActor = nullptr);

    [[nodiscard]] Actor::Ptr getRootNode() const noexcept { return rootNode; }

    [[nodiscard]] bool updateSceneBuffers();
    [[nodiscard]] bool hasBufferUpdateNeeded() const noexcept { return needsBufferUpdate; }
    void markBuffersDirty() noexcept { needsBufferUpdate = true; }

    [[nodiscard]] const SceneDescriptor& getDescriptor() const noexcept { return descriptor; }
    void setDescriptor(const SceneDescriptor& desc) { descriptor = desc; }

    [[nodiscard]] const std::string& getProjectPath() const noexcept { return projectPath; }
    void setProjectPath(std::string_view path) { projectPath = std::string(path); }

private:
    std::string name;
    SceneDescriptor descriptor;

    ActorMap actors;
    ActorNameMap actorsByName;

    std::vector<MeshComponent*> meshComponents;
    std::vector<PhysicsComponent*> physicsComponents;

    std::unique_ptr<physics::PhysicsWorld> physicsWorld;
    Actor::Ptr rootNode;
    std::string projectPath;
    bool needsBufferUpdate{false};

    void registerActor(Actor::Ptr actor);
    void unregisterActor(Actor::Ptr actor);
    void setupDefaultMaterial(class Material& material);

    void registerActorHierarchy(Actor::Ptr actor);
    void unregisterActorHierarchy(Actor::Ptr actor);
};

} // namespace ohao
