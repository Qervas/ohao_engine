// The level module's registrations (level/module.hpp): its components.
#include "level/components.hpp"
#include "level/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerLevelTypes(reflect::TypeRegistry& r) {
    r.type<Level>("Level").component().host(makeComponentOps<Level>())
        .doc("A level file (glTF) placed in the world. The level system expands it into one child entity per node of "
             "the file's default scene (LevelNode), with ids hashed from this entity's id and the node's index; those "
             "entities are the file's and are never saved. Node extras give hints (col, body, mass, fric, rest, trig, "
             "surf, vis, spawn, socket, verb) and components by name.")
        .field("asset", &Level::asset).doc("Project-relative path of the .glb or .gltf file, '/' separators")
            .asset("level")
        .field("lightScale", &Level::lightScale).doc("Multiplier on the file's KHR_lights_punctual intensities")
            .unitless().min(0.0).uiRange(0.0, 10.0)
        .field("collision", &Level::collision).doc("Whether the level's nodes get colliders; false: render only");
    r.type<Socket>("Socket").component().host(makeComponentOps<Socket>())
        .doc("An attach point at the entity's transform (a level's SOCKET_ empties).")
        .field("name", &Socket::name).doc("The socket's name, as attachments refer to it").maxLength(64);
    r.type<LevelNode>("LevelNode").component().runtime().entityNotSaved().host(makeComponentOps<LevelNode>())
        .doc("This entity is a node of a level: made by the level system from the file, never saved, rebuilt from "
             "the file when its Level changes.")
        .field("level", &LevelNode::level).doc("The entity with the Level")
        .field("index", &LevelNode::index).doc("The node's index in the default scene, depth first").unitless();
    r.type<LevelDirty>("LevelDirty").component().runtime()
        .doc("Tag: the Level changed; the level system re-expands it this frame.");
}

} // namespace ohao::ecs
