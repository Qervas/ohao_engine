#pragma once

/**
 * @file serialization.hpp
 * @brief Main include file for the serialization module.
 * 
 * This header includes all the serialization functionality in one place.
 */

#include "scene_serializer.hpp"
#include "actor_serializer.hpp"

namespace ohao {
    /**
     * @namespace ohao::serialization
     * @brief Contains serialization functions and utilities for the OHAO engine.
     */
    namespace serialization {
        /**
         * @brief Create a scene serializer for a given scene.
         * @param scene The scene to serialize/deserialize.
         * @return A SceneSerializer instance configured for the scene.
         */
        inline SceneSerializer createSceneSerializer(Scene* scene) {
            return SceneSerializer(scene);
        }
    }
}

// Convenience typedefs
namespace ohao {
    using SceneSerializer = ohao::SceneSerializer;
    using ActorSerializer = ohao::ActorSerializer;
} 