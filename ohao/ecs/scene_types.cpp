// The registrations of the modules not yet in their own directories (each moves to its module's types.cpp), by
// module; engine/install.cpp's module list calls them.
#include "ecs/scene_types.hpp"
#include "ecs/components.hpp"

namespace ohao::ecs {

// ---- level --------------------------------------------------------------------------------------------------

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

// ---- world_render -------------------------------------------------------------------------------------------

void registerWorldRenderTypes(reflect::TypeRegistry& r) {
    using render::MaterialHandle;
    using render::MeshHandle;

    r.type<MeshHandle>("MeshHandle").runtime()
        .doc("A mesh slot in the GPU render scene. Runtime only: files refer to meshes by source.")
        .field("index", &MeshHandle::index).doc("Slot index; 4294967295 means none").unitless()
        .field("generation", &MeshHandle::generation).doc("Slot generation; a stale handle no longer matches").unitless();
    r.type<MaterialHandle>("MaterialHandle").runtime()
        .doc("A material slot in the GPU render scene. Runtime only.")
        .field("index", &MaterialHandle::index).doc("Slot index; 4294967295 means none").unitless()
        .field("generation", &MaterialHandle::generation).doc("Slot generation").unitless();
    r.type<MeshRenderer>("MeshRenderer").component().runtime().host(makeComponentOps<MeshRenderer>())
        .doc("The GPU mesh and materials an entity renders with. Built from the entity's mesh source.")
        .field("mesh", &MeshRenderer::mesh).doc("Mesh slot")
        .field("materials", &MeshRenderer::materials).doc("One material per submesh, in submesh order")
        .field("visible", &MeshRenderer::visible).doc("Whether the entity is drawn");
    r.type<RenderSlot>("RenderSlot").component().runtime().host(makeComponentOps<RenderSlot>())
        .doc("The entity's instance slot in the GPU render scene. Owned by the render extract.")
        .field("slot", &RenderSlot::slot).doc("Instance slot index").unitless();
    r.type<InstantiateDirty>("InstantiateDirty").component().runtime()
        .doc("Tag: MeshSource or Material changed; instantiate rebuilds the mesh renderer this frame.");
}

} // namespace ohao::ecs
