#pragma once

#include "scene/actor/actor.hpp"
#include "scene/scene.hpp"
#include "scene/component/component.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/light_component.hpp"
#include "physics/components/physics_component.hpp"
#include "core/concepts.hpp"
#include <string_view>
#include <tuple>

namespace ohao {

/**
 * Component Pack System - Template-based safe component management
 * 
 * Usage:
 *   ComponentPack<MeshComponent, MaterialComponent, PhysicsComponent>::applyTo(actor);
 *   
 * This ensures all components are added consistently and safely.
 */

// Component pack class template
template<ComponentType... Components>
class ComponentPack {
public:
    // Apply all components in the pack to an actor
    static void applyTo(Actor::Ptr actor) {
        if (!actor) return;
        (addComponent<Components>(actor), ...);
    }
    
    // Check if actor has all components in the pack
    [[nodiscard]] static bool hasAll(Actor::Ptr actor) {
        if (!actor) return false;
        return (actor->hasComponent<Components>() && ...);
    }
    
    // Get tuple of all components (for advanced usage)
    [[nodiscard]] static auto getAll(Actor::Ptr actor) {
        return std::make_tuple(actor->getComponent<Components>()...);
    }
    
    // Remove all components in the pack
    static void removeFrom(Actor::Ptr actor) {
        if (!actor) return;
        (removeComponent<Components>(actor), ...);
    }
    
    // Get count of components in this pack
    [[nodiscard]] static constexpr size_t count() {
        return sizeof...(Components);
    }
    
private:
    template<ComponentType T>
    static void addComponent(Actor::Ptr actor) {
        if (!actor->hasComponent<T>()) {
            actor->addComponent<T>();
        }
    }
    
    template<ComponentType T>
    static void removeComponent(Actor::Ptr actor) {
        actor->removeComponent<T>();
    }
};

// Pre-defined component packs for common use cases

// Standard visual object pack (mesh + material)
using VisualObjectPack = ComponentPack<MeshComponent, MaterialComponent>;

// Standard physics object pack (mesh + material + physics)  
using PhysicsObjectPack = ComponentPack<MeshComponent, MaterialComponent, PhysicsComponent>;

// Lightweight object pack (just mesh, no physics)
using LightweightObjectPack = ComponentPack<MeshComponent>;

// Full featured object pack (all standard components)
using StandardObjectPack = ComponentPack<MeshComponent, MaterialComponent, PhysicsComponent>;

// Light source pack (mesh + material + light)
using LightSourcePack = ComponentPack<MeshComponent, MaterialComponent, LightComponent>;

// Light only pack (just light component - no mesh/material for invisible lights)
using LightOnlyPack = ComponentPack<LightComponent>;

// Utility class for pack-based actor creation
class ActorFactory {
public:
    template<typename PackType>
    [[nodiscard]] static Actor::Ptr createWithPack(Scene* scene, std::string_view name) {
        auto actor = scene->createActor(std::string(name));
        PackType::applyTo(actor);
        return actor;
    }
    
    // Convenience methods
    [[nodiscard]] static Actor::Ptr createVisualObject(Scene* scene, std::string_view name) {
        return createWithPack<VisualObjectPack>(scene, name);
    }
    
    [[nodiscard]] static Actor::Ptr createPhysicsObject(Scene* scene, std::string_view name) {
        return createWithPack<PhysicsObjectPack>(scene, name);
    }
    
    [[nodiscard]] static Actor::Ptr createStandardObject(Scene* scene, std::string_view name) {
        return createWithPack<StandardObjectPack>(scene, name);
    }
    
    [[nodiscard]] static Actor::Ptr createLightSource(Scene* scene, std::string_view name) {
        return createWithPack<LightSourcePack>(scene, name);
    }
};

} // namespace ohao
