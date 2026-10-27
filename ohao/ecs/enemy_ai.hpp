#pragma once
// The enemy AI (sub-project 7 step 2; docs/specs/2026-10-27-combat-design.md, "Enemies"). Run by
// the combat system at the start of each combat step, before the weapons: every Enemy, in id
// order, sees (a ray from its eye to the player's chest), decides (idle, chase, attack, dead),
// writes its MoveIntent (which the next step's physics walks) and pulls its WeaponTrigger (which
// this step's weapons fire). All its state is in saved components (EnemyState, MoveIntent,
// WeaponTrigger), so a snapshot replays it.

namespace ohao::ecs {

class World;
class PhysicsScene;

void stepEnemies(World& world, PhysicsScene& physics, float dt);

} // namespace ohao::ecs
