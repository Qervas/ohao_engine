// The registrations of the modules not yet in their own directories (each moves to its module's types.cpp), by
// module; engine/install.cpp's module list calls them.
#include "ecs/scene_types.hpp"
#include "ecs/components.hpp"
#include "visual/components.hpp"

namespace ohao::ecs {

// ---- audio --------------------------------------------------------------------------------------------------

void registerAudioTypes(reflect::TypeRegistry& r) {
    r.type<AudioMix>("AudioMix").component().singleton().host(makeComponentOps<AudioMix>())
        .doc("The world's sound bank and mix (one per world): an .audio.json declaring the buses and their voice caps, "
             "the sounds (files, bus, priority, distance shape, how long each holds its voice), the ducks, the "
             "snapshots and the reverb presets. Without one, the default bank (master over sfx, music, ambient).")
        .field("asset", &AudioMix::asset).doc("Project-relative path of the .audio.json file, '/' separators")
            .asset("audio bank");
    r.type<ReverbZone>("ReverbZone").component().host(makeComponentOps<ReverbZone>())
        .doc("A volume whose reverb preset the listener hears while inside it: a box about the entity, in its frame. "
             "Of the zones holding the listener the highest priority wins, then the lowest id; outside them all, the "
             "bank's defaultZone. A volume, not a body: nothing collides with it.")
        .field("preset", &ReverbZone::preset).doc("A reverb preset of the world's .audio.json (room, outdoor, ...)")
            .maxLength(32)
        .field("priority", &ReverbZone::priority).doc("Higher wins where zones overlap").unitless()
        .field("size", &ReverbZone::size).doc("The box's half extents; zero fits it to the entity's mesh").unit("m").min(0.0);
    registerAudioEvents(r);
}

// ---- vfx ----------------------------------------------------------------------------------------------------

void registerVfxTypes(reflect::TypeRegistry& r) {
    r.type<VfxSet>("VfxSet").component().singleton().host(makeComponentOps<VfxSet>())
        .doc("The world's effect set (one per world): a .vfx.json declaring the effects (emitters of closed-form "
             "particles), decals, tracers and which events' cues spawn them. Without one, the default set.")
        .field("asset", &VfxSet::asset).doc("Project-relative path of the .vfx.json file, '/' separators")
            .asset("effect set");
    r.type<VfxEmitter>("VfxEmitter").component().host(makeComponentOps<VfxEmitter>())
        .doc("A placed effect at the entity's pose: while active, rate spawns a second by step count from the step "
             "it became active, or one spawn when it becomes active with rate 0.")
        .field("effect", &VfxEmitter::effect).doc("An effect of the world's .vfx.json").maxLength(64)
        .field("rate", &VfxEmitter::rate).doc("Spawns a second while active; 0: one when it becomes active").unit("Hz")
            .min(0.0).max(1000.0)
        .field("seed", &VfxEmitter::seed).doc("Varies its particles' random draws").unitless()
        .field("active", &VfxEmitter::active).doc("Spawning; turning it on starts the count again");
}

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
