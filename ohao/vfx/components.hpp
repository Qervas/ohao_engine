#pragma once
// The vfx module's components (vfx/module.hpp): the world's effect set (VfxSet) and placed effects (VfxEmitter).
#include "reflect/asset_ref.hpp"

#include <cstdint>
#include <string>
#include "ecs/storage.hpp"   // OHAO_EXTERN_STORAGE: the storages below are compiled once

namespace ohao::ecs {

using AssetRef = reflect::AssetRef;   // as ecs/components.hpp names it

// The world's effect set, one per world (sub-project 12): a `.vfx.json` naming the effects, decals
// and tracers and which combat events spawn them (vfx/effects/vfx_set.hpp). Without one, hosts use the
// default set (today's muzzle, impact and dust presets as data).
struct VfxSet {
    AssetRef asset;   // a .vfx.json, project-relative
};

// A placed effect at its entity's pose (sub-project 12; vfx/vfx.hpp): while `active`, `rate` spawn
// records a second by step count from the step it became active, or one when it becomes active with
// rate 0. `effect` names an effect of the world's set.
struct VfxEmitter {
    std::string effect;
    float rate{0.0f};
    uint32_t seed{0};
    bool active{true};
};

} // namespace ohao::ecs

// The storages of these components, compiled once (vfx/storage.cpp).
OHAO_EXTERN_STORAGE(ohao::ecs::VfxEmitter);
OHAO_EXTERN_STORAGE(ohao::ecs::VfxSet);
