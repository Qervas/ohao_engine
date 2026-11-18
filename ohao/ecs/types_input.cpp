// The core module's registrations (ecs/module.hpp), a part of types.cpp's: the input bindings, the fly controller
// and spawn points.
#include "ecs/components.hpp"
#include "ecs/module.hpp"
#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerCoreTypesInput(reflect::TypeRegistry& r) {
    r.type<ActionBinding>("ActionBinding")
        .doc("An action name bound to a key: held while the key is down.")
        .field("action", &ActionBinding::action).doc("The action's name, as systems and input.inject use it")
        .field("key", &ActionBinding::key).doc("The key or mouse button");
    r.type<AxisBinding>("AxisBinding")
        .doc("An axis name bound to two keys: +1 while positive is down, -1 while negative is, 0 for both or neither.")
        .field("axis", &AxisBinding::axis).doc("The axis's name, as systems and input.inject use it")
        .field("positive", &AxisBinding::positive).doc("The key for +1 (None: no key)")
        .field("negative", &AxisBinding::negative).doc("The key for -1 (None: no key)");
    r.type<InputActions>("InputActions").component().singleton().host(makeComponentOps<InputActions>())
        .doc("The world's controls (one per world): action and axis names over keys. The world's input map is rebuilt "
             "from it whenever it is set, changed or removed.")
        .field("actions", &InputActions::actions).doc("Action bindings; an action with several keys is held while any is")
        .field("axes", &InputActions::axes).doc("Axis bindings");
    r.type<FlyController>("FlyController").component().host(makeComponentOps<FlyController>())
        .doc("Flies the entity from named axes and actions, once per fixed step: forward along its -Z, right along its "
             "+X, up along world +Y; while `look` is held the mouse turns it (yaw about world +Y, pitch about its +X). "
             "On the Camera entity it is a fly camera.")
        .field("moveSpeed", &FlyController::moveSpeed).doc("Speed at an axis value of 1").unit("m/s").min(0.0)
            .uiRange(0.0, 50.0)
        .field("fastMultiplier", &FlyController::fastMultiplier).doc("Speed multiplier while `fast` is held")
            .unitless().min(0.0).uiRange(1.0, 10.0)
        .field("lookDegPerUnit", &FlyController::lookDegPerUnit).doc("Turn per unit of mouse movement while `look` is held")
            .unit("deg").min(0.0).uiRange(0.0, 1.0)
        .field("moveForward", &FlyController::moveForward).doc("Axis moving along the entity's -Z")
        .field("moveRight", &FlyController::moveRight).doc("Axis moving along the entity's +X")
        .field("moveUp", &FlyController::moveUp).doc("Axis moving along world +Y")
        .field("look", &FlyController::look).doc("Action that, while held, lets the mouse turn the entity")
        .field("fast", &FlyController::fast).doc("Action that, while held, multiplies the speed");
    r.type<SpawnPoint>("SpawnPoint").component().host(makeComponentOps<SpawnPoint>())
        .doc("A named place to spawn something, at the entity's world position, facing its -Z. Spawns are taken in "
             "creation order (a level's node order).")
        .field("tag", &SpawnPoint::tag).doc("What spawns here: player, enemy, ...").maxLength(32);
}

} // namespace ohao::ecs
