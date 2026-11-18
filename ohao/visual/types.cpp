// The visual module's registrations (visual/module.hpp): its enums and components.
#include "visual/components.hpp"
#include "visual/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

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
    r.type<Material>("Material").component().host(makeComponentOps<Material>())
        .doc("The material of a mesh without its own materials (primitives): OpenPBR Surface 1.1.1's base, under its "
             "names and defaults. For a model with materials it only sets the legacy RT GI albedo.")
        .field("base_color", &Material::baseColor).doc("The diffuse albedo, and a metal's F0").unit("linear-rgb").color()
            .min(0.0).max(1.0)
        .field("base_weight", &Material::baseWeight).doc("Scales base_color").unitless().min(0.0).max(1.0)
        .field("base_metalness", &Material::baseMetalness).doc("0 = dielectric, 1 = metal").unitless().min(0.0).max(1.0)
        .field("base_diffuse_roughness", &Material::baseDiffuseRoughness)
            .doc("The diffuse's roughness (EON): 0 = Lambert, 1 = the flattest, dustiest").unitless().min(0.0).max(1.0)
        .field("specular_weight", &Material::specularWeight)
            .doc("Scales the dielectric's reflectance at normal incidence (and a metal's whole specular)").unitless()
            .min(0.0)
        .field("specular_color", &Material::specularColor)
            .doc("Tints the dielectric's reflection from above, and a metal's edge (F82)").unit("linear-rgb").color()
            .min(0.0).max(1.0)
        .field("specular_roughness", &Material::specularRoughness).doc("GGX roughness (alpha = its square)").unitless()
            .min(0.0).max(1.0)
        .field("specular_ior", &Material::specularIor).doc("The dielectric's index of refraction").unitless().min(1.0)
            .max(3.0)
        .field("emission_luminance", &Material::emissionLuminance)
            .doc("Emitted radiance, in the engine's units until the camera step (ruling O4)").unitless().min(0.0)
        .field("emission_color", &Material::emissionColor).doc("The emission's colour").unit("linear-rgb").color().min(0.0)
        .field("subsurface_weight", &Material::subsurfaceWeight)
            .doc("Subsurface scattering (skin, wax, marble): the share of the dielectric's diffuse that scatters under "
                 "the surface; 0 = none")
            .unitless().min(0.0).max(1.0)
        .field("alpha_mode", &Material::alphaMode).doc("Opaque, alpha-tested (Mask) or blended (Blend)")
        .field("geometry_opacity", &Material::geometryOpacity)
            .doc("Coverage: Blend's blend factor, Mask's test (cut below 0.5)").unitless().min(0.0).max(1.0)
        .field("specular_roughness_anisotropy", &Material::specularRoughnessAnisotropy)
            .doc("Stretches the specular lobe along the tangent (brushed metal): 0 = isotropic").unitless().min(0.0)
            .max(1.0)
        .field("coat_weight", &Material::coatWeight).doc("A clear coat's coverage (car paint, varnish): 0 = none")
            .unitless().min(0.0).max(1.0)
        .field("coat_color", &Material::coatColor)
            .doc("The coat's transmittance at normal incidence, there and back: tints what is under it").unit("linear-rgb")
            .color().min(0.0).max(1.0)
        .field("coat_roughness", &Material::coatRoughness).doc("The coat's GGX roughness").unitless().min(0.0).max(1.0)
        .field("coat_roughness_anisotropy", &Material::coatRoughnessAnisotropy)
            .doc("Stretches the coat's lobe along the tangent").unitless().min(0.0).max(1.0)
        .field("coat_ior", &Material::coatIor).doc("The coat's index of refraction").unitless().min(1.0).max(3.0)
        .field("coat_darkening", &Material::coatDarkening)
            .doc("How much light the coat reflects back down darkens and saturates the base: 1 = physical, 0 = none")
            .unitless().min(0.0).max(1.0)
        .field("fuzz_weight", &Material::fuzzWeight).doc("A fuzz layer's coverage (velvet, dust, peach skin): 0 = none")
            .unitless().min(0.0).max(1.0)
        .field("fuzz_color", &Material::fuzzColor).doc("The fuzz's colour").unit("linear-rgb").color().min(0.0).max(1.0)
        .field("fuzz_roughness", &Material::fuzzRoughness)
            .doc("The fuzz's roughness: low is a sheen at grazing angles (satin), high a soft haze (velvet)").unitless()
            .min(0.0).max(1.0)
        .field("geometry_tangent_rotation", &Material::geometryTangentRotation)
            .doc("Turns the mesh's tangent about the normal, in turns: the direction anisotropy stretches along")
            .unitless()
        .field("transmission_weight", &Material::transmissionWeight)
            .doc("How much of the dielectric refracts instead of scattering diffusely (glass, water): 0 = opaque")
            .unitless().min(0.0).max(1.0)
        .field("transmission_color", &Material::transmissionColor)
            .doc("The refracted light's colour: at the surface when transmission_depth is 0, else the colour "
                 "light has after travelling transmission_depth inside")
            .unit("linear-rgb").color().min(0.0).max(1.0)
        .field("transmission_depth", &Material::transmissionDepth)
            .doc("The distance inside at which light has transmission_color (Beer-Lambert absorption): 0 = none")
            .unit("m").min(0.0)
        .field("transmission_dispersion_scale", &Material::transmissionDispersionScale)
            .doc("Scales dispersion (the IOR changing with wavelength): 0 = none, 1 = the Abbe number's")
            .unitless().min(0.0).max(1.0)
        .field("transmission_dispersion_abbe_number", &Material::transmissionDispersionAbbeNumber)
            .doc("The glass's Abbe number: lower disperses more (flint ~30, crown ~60)").unitless().min(9.0).max(91.0)
        .field("geometry_thin_walled", &Material::geometryThinWalled)
            .doc("A thin slab with no inside (a window, a soap bubble, a leaf): refraction passes straight through")
        .field("thin_film_weight", &Material::thinFilmWeight)
            .doc("A thin film's coverage over the base (oil on water, soap, anodised metal): 0 = none")
            .unitless().min(0.0).max(1.0)
        .field("thin_film_thickness", &Material::thinFilmThickness)
            .doc("The film's thickness: its interference colours").unit("um").min(0.0).max(2.0)
        .field("thin_film_ior", &Material::thinFilmIor).doc("The film's index of refraction").unitless().min(1.0)
            .max(3.0)
        .field("subsurface_color", &Material::subsurfaceColor)
            .doc("The colour the scattering volume shows (its multiple-scattering albedo)").unit("linear-rgb").color()
            .min(0.0).max(1.0)
        .field("subsurface_radius", &Material::subsurfaceRadius)
            .doc("The mean free path: how far light travels under the surface (skin ~0.01, marble ~0.05)").unit("m")
            .min(0.0)
        .field("subsurface_radius_scale", &Material::subsurfaceRadiusScale)
            .doc("subsurface_radius per channel (red travels furthest in skin)").unit("linear-rgb").min(0.0).max(1.0)
        .field("subsurface_scatter_anisotropy", &Material::subsurfaceScatterAnisotropy)
            .doc("The volume's Henyey-Greenstein anisotropy: > 0 forward, < 0 backward").unitless().min(-1.0).max(1.0);
    r.type<Camera>("Camera").component().singleton().host(makeComponentOps<Camera>())
        .doc("The scene camera (one per world). Its pose is the entity's world transform: it looks along -Z with +Y "
             "up; roll is ignored.")
        .field("fovDeg", &Camera::fovDeg).doc("Vertical field of view").unit("deg").angle().min(1.0).max(179.0)
        .field("nearPlane", &Camera::nearPlane).doc("Near clip distance (raster passes)").unit("m").min(0.0001)
        .field("farPlane", &Camera::farPlane).doc("Far clip distance (raster passes); keep it above nearPlane")
            .unit("m").min(0.001)
        .field("active", &Camera::active).doc("false: the host keeps its own camera");
    r.type<Environment>("Environment").component().singleton().host(makeComponentOps<Environment>())
        .doc("What lights and backs the scene from afar (one per world): an equirectangular HDR image, else the "
             "physical sky (Hillaire) lit by the scene's first directional light. A world without one has the sky.")
        .field("hdri", &Environment::hdri).doc("Project-relative path of the .hdr image; null for the sky")
            .asset("environment image")
        .field("intensity", &Environment::intensity).doc("Multiplier on the environment's radiance").unitless().min(0.0)
            .uiRange(0.0, 10.0)
        .field("sky", &Environment::sky)
            .doc("Without an hdri: the physical sky for the scene's sun (none without a directional light); false: "
                 "no environment (black)");
    r.type<InstantiateError>("InstantiateError").component().runtime().host(makeComponentOps<InstantiateError>())
        .doc("Why instantiate could not build the entity's mesh. The entity then has no MeshRenderer.")
        .field("code", &InstantiateError::code).doc("E_ASSET_NOT_FOUND, E_ASSET_INVALID or E_MESH_INVALID")
        .field("message", &InstantiateError::message).doc("What went wrong, for people and agents");
}

} // namespace ohao::ecs
