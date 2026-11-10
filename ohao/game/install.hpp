#pragma once
// A game on a World (step 3j): installed with a record of what it added, removed by that record, and swapped for a
// newer build of itself in place (game.reload).
//
// A reload keeps the world as it is. What the engine holds of the game's stays and is read by the new build in place:
// its components (EnTT's pools, never removed), its channels and their events, and its snapshot hooks' states, handed
// from the old build's hooks to the new's. So every type that crosses must keep its layout (core/layout_key.hpp): the
// reflected types (TypeInfo::layout) and each hook's state (SnapshotHook::layout). A build that changes one is
// refused, the old build left installed: such an edit needs a new session. Reflected docs and limits keep the old
// build's until then. The old build stays loaded (game/loader.hpp).
#include <cstdint>
#include <string>
#include <vector>

namespace ohao::ecs {
class World;
}
namespace ohao::reflect {
class TypeRegistry;
}

namespace ohao::game {

class Module;

// Installs `module` on `world` (after engine::installEngine), recording what it added: simulate and sync systems,
// snapshot hooks, animation drivers, presenters (world_render/present.hpp) and the physics control rule. If install
// throws, what it added so far is removed before the exception goes on.
void installGame(ecs::World& world, const Module& module);
// Removes what installGame recorded, then calls the module's uninstall for its own. Between steps.
void uninstallGame(ecs::World& world);
// The game installGame put on the world, or null.
[[nodiscard]] const Module* installedGame(const ecs::World& world);

struct ReloadReport {
    std::vector<std::string> carried;   // the snapshot hooks whose state the new build took
    std::vector<std::string> fresh;     // the new build's hooks the old had not: their state starts anew
};

// Swaps the world's game for `next`, a newer build of the installed one, between steps. `types` is the registry the
// world's data was made with (the engine's and the old build's). False with `error` and the old build installed
// again (its state restored) when a type's layout or a hook's state layout differs, a hook declares no layout, or
// next's install throws.
bool reloadGame(ecs::World& world, const reflect::TypeRegistry& types, const Module& next, ReloadReport& report,
                std::string& error);

} // namespace ohao::game
