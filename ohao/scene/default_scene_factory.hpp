#pragma once

#include "scene/scene.hpp"
#include "scene/component/component_factory.hpp"
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
    [[nodiscard]] static std::unique_ptr<Scene> createBlenderLikeScene();
    
    /**
     * Create an empty scene with just basic lighting
     */
    [[nodiscard]] static std::unique_ptr<Scene> createEmptyScene();
    
    /**
     * Create a physics test scene with multiple objects for testing
     */
    [[nodiscard]] static std::unique_ptr<Scene> createPhysicsTestScene();
    
private:
    static void setupDefaultLighting(Scene* scene);
    static void setupGroundPlane(Scene* scene);
    static void setupDefaultCamera(Scene* scene); // For future camera entity system
};

} // namespace ohao
