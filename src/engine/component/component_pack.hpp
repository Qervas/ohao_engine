#pragma once

#include "engine/actor/actor.hpp"
#include "engine/component/component.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "renderer/components/light_component.hpp"
#include "physics/components/physics_component.hpp"
#include <type_traits>
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

// Helper trait to detect component types
template<typename T>
struct is_component : std::is_base_of<Component, T> {};

// Component pack class template
template<typename... Components>
class ComponentPack {
    static_assert((is_component<Components>::value && ...), "All types must be components");
    
public:
    // Apply all components in the pack to an actor
    static void applyTo(Actor::Ptr actor) {
        if (!actor) return;
        (addComponent<Components>(actor), ...); // Fold expression (C++17)
    }
    
    // Check if actor has all components in the pack
    static bool hasAll(Actor::Ptr actor) {
        if (!actor) return false;
        return (actor->hasComponent<Components>() && ...);
    }
    
    // Get tuple of all components (for advanced usage)
    static auto getAll(Actor::Ptr actor) {
        return std::make_tuple(actor->getComponent<Components>()...);
    }
    
    // Remove all components in the pack
    static void removeFrom(Actor::Ptr actor) {
        if (!actor) return;
        (removeComponent<Components>(actor), ...);
    }
    
    // Get count of components in this pack
    static constexpr size_t count() {
        return sizeof...(Components);
    }
    
private:
    template<typename T>
    static void addComponent(Actor::Ptr actor) {
        if (!actor->hasComponent<T>()) {
            actor->addComponent<T>();
        }
    }
    
    template<typename T>
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
    static Actor::Ptr createWithPack(Scene* scene, const std::string& name) {
        auto actor = scene->createActor(name);
        PackType::applyTo(actor);
        return actor;
    }
    
    // Convenience methods
    static Actor::Ptr createVisualObject(Scene* scene, const std::string& name) {
        return createWithPack<VisualObjectPack>(scene, name);
    }
    
    static Actor::Ptr createPhysicsObject(Scene* scene, const std::string& name) {
        return createWithPack<PhysicsObjectPack>(scene, name);
    }
    
    static Actor::Ptr createStandardObject(Scene* scene, const std::string& name) {
        return createWithPack<StandardObjectPack>(scene, name);
    }
    
    static Actor::Ptr createLightSource(Scene* scene, const std::string& name) {
        return createWithPack<LightSourcePack>(scene, name);
    }
};

} // namespace ohao