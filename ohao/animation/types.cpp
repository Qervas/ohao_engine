// The animation module's registrations (animation/module.hpp): its components and its event's payload.
#include "animation/components.hpp"
#include "animation/events.hpp"
#include "animation/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerAnimationTypes(reflect::TypeRegistry& r) {
    r.type<SkinnedMesh>("SkinnedMesh").component().host(makeComponentOps<SkinnedMesh>())
        .doc("A skinned mesh: the entity's MeshSource (a ModelAsset with `mesh`) is deformed by a skin of a glTF file, "
             "posed by the entity's Animator. The instance gets its own deformed copy of the mesh.")
        .field("file", &SkinnedMesh::file).doc("The glTF (.glb) holding the skin").asset("skin")
        .field("skin", &SkinnedMesh::skin).doc("The skin's name or \"#<index>\"; empty: the file's first skin")
            .maxLength(256);
    r.type<AnimLayerState>("AnimLayerState")
        .doc("One layer of an Animator: its state, the steps in it, and the fade from the state it left.")
        .field("state", &AnimLayerState::state).doc("The layer's current state (empty: its initial state)").maxLength(256)
        .field("steps", &AnimLayerState::steps).doc("Simulation steps since entering it").unitless()
        .field("from", &AnimLayerState::from).doc("The state faded from; empty when not fading").maxLength(256)
        .field("fromSteps", &AnimLayerState::fromSteps).doc("Its steps").unitless()
        .field("fade", &AnimLayerState::fade).doc("The fade's length in steps (0: none)").unitless()
        .field("faded", &AnimLayerState::faded).doc("Steps of the fade done").unitless();
    r.type<Animator>("Animator").component().host(makeComponentOps<Animator>())
        .doc("Poses an entity by an animation graph (.animgraph.json): each layer's state machine advances one step a "
             "simulation step (integers only, so a snapshot holds it exactly). The pose is presentation: never saved or "
             "hashed.")
        .field("graph", &Animator::graph).doc("The animation graph file").asset("animation graph")
        .field("layers", &Animator::layers).doc("One a graph layer, in order; empty: filled at the first step");
    r.type<JointAttach>("JointAttach").component().host(makeComponentOps<JointAttach>())
        .doc("Drawn at a joint of its parent's pose (times its own Transform): a rifle in a hand. Presentation: its "
             "Transform, and what the step sees, do not follow the joint.")
        .field("joint", &JointAttach::joint).doc("The joint's name in the parent's skeleton").maxLength(256);

    r.type<ClipEvent>("ClipEvent").event()
        .doc("anim.clip_event: an event a layer's clip fired in the step (mag_out, mag_in, bolt, body_fall, foot_l, ...)")
        .field("entity", &ClipEvent::entity).entity().doc("The animated entity")
        .field("layer", &ClipEvent::layer).doc("The graph layer").unitless()
        .field("clip", &ClipEvent::clip).doc("The clip that fired it")
        .field("name", &ClipEvent::name).doc("The event's name in the clip");
}

} // namespace ohao::ecs
