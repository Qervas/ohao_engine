// The registrations of the modules not yet in their own directories (each moves to its module's types.cpp), by
// module; engine/install.cpp's module list calls them.
#include "ecs/scene_types.hpp"
#include "ecs/components.hpp"

namespace ohao::ecs {

// ---- world_render -------------------------------------------------------------------------------------------

void registerWorldRenderTypes(reflect::TypeRegistry& r) {
    using render::MaterialHandle;
    using render::MeshHandle;

    r.type<MeshHandle>("MeshHandle").runtime()
        .doc("A mesh slot in the GPU render scene. Runtime only: files refer to meshes by source.")
        .field("index", &MeshHandle::index).doc("Slot index; 4294967295 means none").unitless()
        .field("generation", &MeshHandle::generation).doc("Slot generation; a stale handle no longer matches").unitless();
    r.type<MaterialHandle>("MaterialHandle").runtime()
        .doc("A material slot in the GPU render scene. Runtime only.")
        .field("index", &MaterialHandle::index).doc("Slot index; 4294967295 means none").unitless()
        .field("generation", &MaterialHandle::generation).doc("Slot generation").unitless();
    r.type<MeshRenderer>("MeshRenderer").component().runtime().host(makeComponentOps<MeshRenderer>())
        .doc("The GPU mesh and materials an entity renders with. Built from the entity's mesh source.")
        .field("mesh", &MeshRenderer::mesh).doc("Mesh slot")
        .field("materials", &MeshRenderer::materials).doc("One material per submesh, in submesh order")
        .field("visible", &MeshRenderer::visible).doc("Whether the entity is drawn");
    r.type<RenderSlot>("RenderSlot").component().runtime().host(makeComponentOps<RenderSlot>())
        .doc("The entity's instance slot in the GPU render scene. Owned by the render extract.")
        .field("slot", &RenderSlot::slot).doc("Instance slot index").unitless();
    r.type<InstantiateDirty>("InstantiateDirty").component().runtime()
        .doc("Tag: MeshSource or Material changed; instantiate rebuilds the mesh renderer this frame.");
}

} // namespace ohao::ecs
