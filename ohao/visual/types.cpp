// The visual module's registrations (visual/module.hpp): its enums and components.
#include "visual/components.hpp"
#include "visual/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

// The parts of its registrations, each its own unit (they compile in parallel), in this order.
void registerVisualTypesMesh(reflect::TypeRegistry& r);       // ModelAsset, the prims, MeshSource (types_mesh.cpp)
void registerVisualTypesMaterial(reflect::TypeRegistry& r);   // Material (types_material.cpp)
void registerVisualTypesView(reflect::TypeRegistry& r);       // Camera, Environment, InstantiateError (types_view.cpp)

void registerVisualTypes(reflect::TypeRegistry& r) {
    r.enumeration<LightType>("LightType")
        .doc("Light shape. Position and direction come from the entity's world transform; "
             "the direction is the entity's -Z axis.")
        .value("Point", LightType::Point, "Omnidirectional point light at the entity's position")
        .value("Directional", LightType::Directional, "Infinitely distant light (a sun) shining along -Z")
        .value("Spot", LightType::Spot, "Cone of light along -Z, shaped by innerConeDeg/outerConeDeg")
        .value("AreaRect", LightType::AreaRect, "Rectangle spanned by areaEdge1 and areaEdge2, emitting along -Z");
    r.enumeration<MaterialAlpha>("MaterialAlpha")
        .doc("A material's alpha mode (glTF's alphaMode)")
        .value("Opaque", MaterialAlpha::Opaque, "Opaque: opacity is ignored")
        .value("Mask", MaterialAlpha::Mask, "Alpha-tested: cut where opacity is below 0.5, in raster and every ray")
        .value("Blend", MaterialAlpha::Blend,
               "Blended by opacity in the sorted forward pass; casts no shadow and bounces no GI in the hybrid frame");
    r.type<Light>("Light").component().host(makeComponentOps<Light>())
        .doc("A light source. Position and direction come from the transform (direction = -Z).")
        .field("type", &Light::type).doc("Light shape")
        .field("color", &Light::color).doc("Emitted color").unit("linear-rgb").color().min(0.0)
        .field("intensity", &Light::intensity).doc("Brightness multiplier on color").unitless().min(0.0).uiRange(0.0, 100.0)
        .field("range", &Light::range).doc("Distance at which a Point/Spot light's contribution reaches zero").unit("m").min(0.0)
        .field("radius", &Light::radius).doc("Emitter radius; larger values give softer shadows").unit("m").min(0.0)
        .field("innerConeDeg", &Light::innerConeDeg).doc("Spot: full-intensity cone angle").unit("deg").angle().min(0.0).max(180.0)
        .field("outerConeDeg", &Light::outerConeDeg).doc("Spot: cone angle where intensity reaches zero").unit("deg").angle().min(0.0).max(180.0)
        .field("areaEdge1", &Light::areaEdge1).doc("AreaRect: first edge vector, local space").unit("m")
        .field("areaEdge2", &Light::areaEdge2).doc("AreaRect: second edge vector, local space").unit("m")
        .field("castShadows", &Light::castShadows)
            .doc("Whether geometry blocks this light in the raster frame: RT shadows (one shadow-mask channel "
                 "each, for the first 4 lights that cast them), or the cascaded shadow map for a directional "
                 "light when RT shadows are off. False lights through everything. The path tracer always "
                 "traces visibility");
    registerVisualTypesMesh(r);
    registerVisualTypesMaterial(r);
    registerVisualTypesView(r);
}

} // namespace ohao::ecs
