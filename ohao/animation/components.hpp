#pragma once
// The animation module's components (animation/module.hpp; sub-project 10, docs/specs/2026-10-28-animation-design.md):
// what poses an entity (Animator), the skin it deforms (SkinnedMesh) and what is drawn at a joint (JointAttach).
#include "reflect/asset_ref.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include "ecs/storage.hpp"   // OHAO_EXTERN_STORAGE: the storages below are compiled once

namespace ohao::ecs {

using AssetRef = reflect::AssetRef;   // as ecs/components.hpp names it

// A skinned mesh: the entity's MeshSource (a ModelAsset with `mesh`, one glTF mesh at its own
// origin) is deformed by a skin of a glTF file. The entity's Animator poses it; the renderer
// gives the instance its own deformed copy of the mesh (render extract, RenderScene).
struct SkinnedMesh {
    AssetRef file;      // the glTF holding the skin (usually the mesh's own file)
    std::string skin;   // the skin's name, or "#<index>"; empty: the file's first skin
};

// One layer of an Animator: its state, how many steps it has been in it, and the fade from the
// state it left. Integers only, so clip time never drifts and a snapshot holds it exactly.
struct AnimLayerState {
    std::string state;          // a state of the layer (empty: the layer's initial state)
    uint32_t steps{0};          // steps since entering it
    std::string from;           // the state faded from ("" when not fading)
    uint32_t fromSteps{0};      // its steps
    uint32_t fade{0};           // the fade's length in steps (0: none)
    uint32_t faded{0};          // steps of it done
};

// What poses an entity: an animation graph (`.animgraph.json`) and each layer's state, advanced
// one step a simulation step by the `animation` simulate system (animation/animation.hpp). Saved and
// hashed; the pose it gives is presentation (AnimPose, runtime).
struct Animator {
    AssetRef graph;
    std::vector<AnimLayerState> layers;   // one a graph layer, in order; filled at the first step
};

// A child drawn at a joint of its parent's pose (times its own Transform): a rifle in a hand.
// Presentation: its RenderPose follows the joint; its Transform, and what the step sees, do not.
struct JointAttach {
    std::string joint;
};

} // namespace ohao::ecs

// The storages of these components, compiled once (animation/storage.cpp).
OHAO_EXTERN_STORAGE(ohao::ecs::Animator);
OHAO_EXTERN_STORAGE(ohao::ecs::JointAttach);
OHAO_EXTERN_STORAGE(ohao::ecs::SkinnedMesh);
