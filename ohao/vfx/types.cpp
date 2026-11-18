// The vfx module's registrations (vfx/module.hpp): its components.
#include "vfx/components.hpp"
#include "vfx/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

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

} // namespace ohao::ecs
