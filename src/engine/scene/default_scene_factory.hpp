#pragma once

#include "engine/scene/scene.hpp"
#include "engine/component/component_factory.hpp"
#include <memory>

namespace ohao {

/**
 * Factory for creating default scene setups
 */
class DefaultSceneFactory {
public:
    /**
     * Create a default scene similar to Blender's startup scene
     * Contains: Default sphere (with physics), ground plane, directional light
     */
    static std::unique_ptr<Scene> createBlenderLikeScene();
    
    /**
     * Create an empty scene with just basic lighting
     */
    static std::unique_ptr<Scene> createEmptyScene();
    
    /**
     * Create a physics test scene with multiple objects for testing
     */
    static std::unique_ptr<Scene> createPhysicsTestScene();
    
private:
    static void setupDefaultLighting(Scene* scene);
    static void setupGroundPlane(Scene* scene);
    static void setupDefaultCamera(Scene* scene); // For future camera entity system
};

} // namespace ohao