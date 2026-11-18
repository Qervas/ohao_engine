// The visual module's registrations (visual/module.hpp), a part of types.cpp's: ModelAsset, the primitives and
// MeshSource.
#include "visual/components.hpp"
#include "visual/module.hpp"
#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerVisualTypesMesh(reflect::TypeRegistry& r) {
    // Mesh sources: an asset or a parametric primitive.
    r.type<ModelAsset>("ModelAsset")
        .doc("A model file (glb, gltf) loaded through the asset resolver; its own "
             "materials are used.")
        .field("asset", &ModelAsset::asset).doc("Project-relative path of the model file, '/' separators").asset("model")
        .field("mesh", &ModelAsset::mesh)
            .doc("One mesh of a glTF file, by name or '#<index>', at its own origin with only its own materials; "
                 "empty: the whole file, every node's meshes merged in place")
            .omitDefault();
    r.type<SpherePrim>("SpherePrim")
        .doc("A UV sphere around the origin.")
        .field("radius", &SpherePrim::radius).doc("Radius").unit("m").min(0.0)
        .field("sectors", &SpherePrim::sectors).doc("Segments around the equator").unitless().min(3).max(1024)
        .field("stacks", &SpherePrim::stacks).doc("Segments from pole to pole").unitless().min(2).max(1024);
    r.type<CubePrim>("CubePrim")
        .doc("A cube around the origin.")
        .field("size", &CubePrim::size).doc("Edge length").unit("m").min(0.0);
    r.type<PlatformPrim>("PlatformPrim")
        .doc("A flat box (a floor slab or platform) around the origin.")
        .field("width", &PlatformPrim::width).doc("Size along X").unit("m").min(0.0)
        .field("height", &PlatformPrim::height).doc("Size along Y").unit("m").min(0.0)
        .field("depth", &PlatformPrim::depth).doc("Size along Z").unit("m").min(0.0);
    r.type<CylinderPrim>("CylinderPrim")
        .doc("A capped cylinder along Y, centered on the origin.")
        .field("radius", &CylinderPrim::radius).doc("Radius").unit("m").min(0.0)
        .field("height", &CylinderPrim::height).doc("Height along Y").unit("m").min(0.0)
        .field("segments", &CylinderPrim::segments).doc("Segments around the axis").unitless().min(3).max(1024);
    r.type<ConePrim>("ConePrim")
        .doc("A capped cone along Y, centered on the origin.")
        .field("radius", &ConePrim::radius).doc("Base radius").unit("m").min(0.0)
        .field("height", &ConePrim::height).doc("Height along Y").unit("m").min(0.0)
        .field("segments", &ConePrim::segments).doc("Segments around the axis").unitless().min(3).max(1024);
    r.type<QuadPrim>("QuadPrim")
        .doc("One quad a-b-c-d (triangles a-b-c, a-c-d) with a flat normal and a vertex color.")
        .field("a", &QuadPrim::a).doc("First corner").unit("m")
        .field("b", &QuadPrim::b).doc("Second corner").unit("m")
        .field("c", &QuadPrim::c).doc("Third corner").unit("m")
        .field("d", &QuadPrim::d).doc("Fourth corner").unit("m")
        .field("normal", &QuadPrim::normal).doc("Normal of every vertex").unitless()
        .field("color", &QuadPrim::color).doc("Vertex color").unit("linear-rgb").color().min(0.0);
    r.type<BoxPrim>("BoxPrim")
        .doc("An axis-aligned box from lo to hi: 6 faces with flat normals and a vertex color.")
        .field("lo", &BoxPrim::lo).doc("Minimum corner").unit("m")
        .field("hi", &BoxPrim::hi).doc("Maximum corner").unit("m")
        .field("color", &BoxPrim::color).doc("Vertex color").unit("linear-rgb").color().min(0.0);
    r.type<MeshSource>("MeshSource").component().host(makeComponentOps<MeshSource>())
        .doc("Where the entity's mesh comes from: a model asset or a primitive. Instantiate builds the GPU mesh; "
             "entities with equal sources share one mesh.")
        .field("source", &MeshSource::source).doc("Exactly one of the alternatives, as {\"Name\": {...}}");
}

} // namespace ohao::ecs
