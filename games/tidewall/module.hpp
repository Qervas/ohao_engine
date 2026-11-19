#pragma once
// Tidewall (docs/specs/2026-11-18-tidewall-design.md): the mission demo, a game on the engine's modules. Its level is
// built from games/tidewall/level/layout.json by tools/level/build_level.py; its scenes are under assets/tidewall/.
#include "game/api.hpp"

namespace ohao::tidewall {

[[nodiscard]] const game::Module& module();

} // namespace ohao::tidewall
