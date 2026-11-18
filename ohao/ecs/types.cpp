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

// The parts of its registrations, each its own unit (they compile in parallel), in this order.
void registerCoreTypesInput(reflect::TypeRegistry& r);     // input, fly controller, spawn points (types_input.cpp)
void registerCoreTypesRuntime(reflect::TypeRegistry& r);   // runtime components, RestoreEvent (types_runtime.cpp)

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
    registerCoreTypesInput(r);
    registerCoreTypesRuntime(r);
}

} // namespace ohao::ecs
