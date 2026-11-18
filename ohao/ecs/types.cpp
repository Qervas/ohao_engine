// The core module's registrations (ecs/module.hpp, registerCoreTypes): what the World itself knows.
#include "ecs/components.hpp"
#include "ecs/module.hpp"
#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerKeyEnum(reflect::TypeRegistry& r) {
    auto keys = r.enumeration<Key>("Key").doc("A keyboard key or mouse button an action or axis is bound to");
    for (const KeyName& k : keyNames()) {
        const std::string name(k.name);
        const std::string doc = k.key == Key::None             ? "Nothing (an unbound side of an axis)"
                                : name.rfind("Mouse", 0) == 0 ? "The " + name.substr(5) + " mouse button"
                                                              : "The " + name + " key";
        keys.value(k.name, k.key, doc);
    }
}

void registerCoreTypes(reflect::TypeRegistry& r) {
    registerKeyEnum(r);
    r.type<Name>("Name").component().alwaysPresent().host(makeComponentOps<Name>())
        .doc("Display name for people and agents. Not unique; entities are identified by id.")
        .field("value", &Name::value).doc("The name");
    r.type<Transform>("Transform").component().alwaysPresent().host(makeComponentOps<Transform>())
        .doc("Local transform relative to the parent (or the world for a root entity). "
             "World matrix = parent world * T * R * S.")
        .field("position", &Transform::position).doc("Local translation").unit("m")
        .field("rotation", &Transform::rotation).doc("Local rotation as a unit quaternion, written [x, y, z, w]").unitless()
        .field("scale", &Transform::scale).doc("Local scale per axis; a negative value mirrors").unitless();
    r.type<Parent>("Parent").component().host(makeComponentOps<Parent>())
        .doc("Attaches the entity to a parent; its Transform is then relative to the parent. "
             "Cycles are rejected.")
        .field("entity", &Parent::entity).doc("The parent entity's id; null makes the entity a root");
    r.type<Spin>("Spin").component().host(makeComponentOps<Spin>())
        .doc("Turns the entity about an axis of its own frame at a constant rate (the spin system, which runs "
             "before the transform system on every world update).")
        .field("axis", &Spin::axis).doc("Rotation axis in the entity's local frame; any length (normalized), "
                                        "zero for no rotation").unitless()
        .field("degreesPerSecond", &Spin::degreesPerSecond).doc("Turn rate; negative turns the other way")
            .unit("deg/s").uiRange(-720.0, 720.0);
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
    r.type<Children>("Children").component().runtime().host(makeComponentOps<Children>())
        .doc("The entity's children in attach order. Derived: rebuilt from Parent.")
        .field("entities", &Children::entities).doc("Child entities");
    r.type<WorldTransform>("WorldTransform").component().runtime().host(makeComponentOps<WorldTransform>())
        .doc("World matrices computed by the transform system from Transform and Parent.")
        .field("world", &WorldTransform::world).doc("Object-to-world matrix this frame, column-major").unitless()
        .field("prevWorld", &WorldTransform::prevWorld).doc("Last frame's object-to-world matrix (motion vectors)").unitless();
    r.type<CreationOrder>("CreationOrder").component().runtime().host(makeComponentOps<CreationOrder>())
        .doc("Monotonic creation sequence. Orders draws, lights and file entities deterministically.")
        .field("seq", &CreationOrder::seq).doc("Sequence number, never recycled");
    r.type<PreservedComponent>("PreservedComponent").runtime()
        .doc("One unknown component kept from a scene file.")
        .field("name", &PreservedComponent::name).doc("Component name as written in the file")
        .field("version", &PreservedComponent::version).doc("Its version from the file header (1 if absent)").unitless()
        .field("json", &PreservedComponent::json).doc("Its value as canonical compact JSON");
    r.type<PreservedComponents>("PreservedComponents").component().runtime().host(makeComponentOps<PreservedComponents>())
        .doc("Components a scene file carried that this build does not know; written back on save.")
        .field("entries", &PreservedComponents::entries).doc("The kept components, in file order");
    r.type<TransformDirty>("TransformDirty").component().runtime()
        .doc("Tag: the local transform changed; world matrices are recomputed this frame.");
    r.type<RenderDirty>("RenderDirty").component().runtime()
        .doc("Tag: the entity's render instance is re-uploaded this frame.");
    r.type<MovedLastFrame>("MovedLastFrame").component().runtime()
        .doc("Tag: moved last frame; uploaded once more so prevWorld catches up.");
    r.type<RestoreEvent>("RestoreEvent").event()
        .doc("world.restore: a restore (world.restore, a round's restart) put the world back to another moment; in "
             "the history only, before the restored next-step events")
        .field("step", &RestoreEvent::step).doc("The World's step the world went back to");
}

} // namespace ohao::ecs
