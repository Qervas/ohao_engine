// The core module's registrations (ecs/module.hpp), a part of types.cpp's: its runtime components and RestoreEvent.
#include "ecs/components.hpp"
#include "ecs/module.hpp"
#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerCoreTypesRuntime(reflect::TypeRegistry& r) {
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
