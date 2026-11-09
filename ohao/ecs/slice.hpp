#pragma once
// The slice, the game the engine is built with (step 3c; docs/plans/2026-11-09-step3-engine-game-boundary.md):
// combat with its rounds, enemies and grenades, its channels (`slice.*`), the first-person view model and the arms
// and enemy animation drivers. Installed apart from the engine (engine::installEngine installs no game system), so
// a world without it has none of them. Step 3i moves it to games/slice behind the game API.
namespace ohao::ecs {

class World;

// After the engine's systems (physics and animation installed first). A second call does nothing.
void installSlice(World& world);

} // namespace ohao::ecs
