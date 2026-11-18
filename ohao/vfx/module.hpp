#pragma once
// The vfx module, "vfx": the `vfx` simulate system and its spawn records (vfx/vfx.hpp), its components
// (vfx/components.hpp). Below it, in effects/: the effect set and the closed forms (ohao_vfx_effects), which the
// renderer draws with. vfx.info is the session's (it reports the host's GPU frame).
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& vfxModule();
// Its registrations (vfx/types.cpp).
void registerVfxTypes(reflect::TypeRegistry& types);

} // namespace ohao::ecs
