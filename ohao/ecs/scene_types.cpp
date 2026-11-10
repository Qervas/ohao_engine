#include "ecs/scene_types.hpp"
#include "ecs/components.hpp"

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

void registerSceneTypes(reflect::TypeRegistry& r) {
    using render::MaterialHandle;
    using render::MeshHandle;

    r.enumeration<LightType>("LightType")
        .doc("Light shape. Position and direction come from the entity's world transform; "
             "the direction is the entity's -Z axis.")
        .value("Point", LightType::Point, "Omnidirectional point light at the entity's position")
        .value("Directional", LightType::Directional, "Infinitely distant light (a sun) shining along -Z")
        .value("Spot", LightType::Spot, "Cone of light along -Z, shaped by innerConeDeg/outerConeDeg")
        .value("AreaRect", LightType::AreaRect, "Rectangle spanned by areaEdge1 and areaEdge2, emitting along -Z");

    registerKeyEnum(r);

    r.enumeration<MaterialAlpha>("MaterialAlpha")
        .doc("A material's alpha mode (glTF's alphaMode)")
        .value("Opaque", MaterialAlpha::Opaque, "Opaque: opacity is ignored")
        .value("Mask", MaterialAlpha::Mask, "Alpha-tested: cut where opacity is below 0.5, in raster and every ray")
        .value("Blend", MaterialAlpha::Blend,
               "Blended by opacity in the sorted forward pass; casts no shadow and bounces no GI in the hybrid frame");

    r.enumeration<ColliderShape>("ColliderShape")
        .doc("A collider's shape, fitted to the entity's mesh unless a size is given.")
        .value("Box", ColliderShape::Box, "The mesh's bounding box")
        .value("Sphere", ColliderShape::Sphere, "Around the bounding box's centre, to the farthest vertex")
        .value("Capsule", ColliderShape::Capsule, "Along the bounding box's longest axis")
        .value("Convex", ColliderShape::Convex, "The convex hull of the mesh's vertices")
        .value("Mesh", ColliderShape::Mesh, "The mesh's triangles (static and kinematic bodies; a dynamic one is made convex)")
        .value("None", ColliderShape::None, "No collision");
    r.enumeration<Motion>("Motion")
        .doc("How a rigid body moves.")
        .value("Static", Motion::Static, "Never moves")
        .value("Dynamic", Motion::Dynamic, "Moved by the simulation; writes its entity's Transform each step")
        .value("Kinematic", Motion::Kinematic, "Moved by its transform; dynamic bodies collide with it");
    r.enumeration<JointType>("JointType")
        .doc("What a joint lets its two bodies do.")
        .value("Fixed", JointType::Fixed, "Nothing: welded as they were built")
        .value("Point", JointType::Point, "Turn freely about the anchor (ball and socket)")
        .value("Hinge", JointType::Hinge, "Turn about the axis through the anchor (a door)")
        .value("Slider", JointType::Slider, "Slide along the axis without turning")
        .value("SwingTwist", JointType::SwingTwist, "Swing within a cone about the axis and twist about it (a shoulder)")
        .value("Distance", JointType::Distance, "Keep the anchors within a distance (a rope, a chain link)");

    // ---- authoring components (saved in scene files) -----------------------------------

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
        .doc("A model file (glb, gltf, fbx, obj, dae, blend, 3ds, ply) loaded through the asset resolver; its own "
             "materials are used.")
        .field("asset", &ModelAsset::asset).doc("Project-relative path of the model file, '/' separators")
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
        .field("intensity", &Environment::intensity).doc("Multiplier on the environment's radiance").unitless().min(0.0)
            .uiRange(0.0, 10.0)
        .field("sky", &Environment::sky)
            .doc("Without an hdri: the physical sky for the scene's sun (none without a directional light); false: "
                 "no environment (black)");

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

    r.type<PlayerController>("PlayerController").component().host(makeComponentOps<PlayerController>())
        .doc("A first-person player: a capsule standing on the entity's position (its feet), turned about +Y. Once per "
             "fixed step, before bodies step, it walks from the named axes and actions (forward along its -Z, right "
             "along its +X), jumps, climbs steps and slopes, and pushes dynamic bodies. It spawns at the first "
             "SpawnPoint tagged \"player\"; triggers and rays see it.")
        .field("radius", &PlayerController::radius).doc("Capsule radius").unit("m").min(0.05).max(2.0)
        .field("height", &PlayerController::height).doc("Capsule height, feet to top").unit("m").min(0.2).max(5.0)
        .field("eyeHeight", &PlayerController::eyeHeight).doc("The eyes above the feet (the first-person camera)")
            .unit("m").min(0.0).max(5.0)
        .field("walkSpeed", &PlayerController::walkSpeed).doc("Speed at a move axis of 1").unit("m/s").min(0.0).max(50.0)
        .field("sprintSpeed", &PlayerController::sprintSpeed).doc("Speed while `sprint` is held").unit("m/s").min(0.0)
            .max(50.0)
        .field("acceleration", &PlayerController::acceleration).doc("How fast the horizontal velocity reaches the wanted one")
            .unit("m/s2").min(0.0).max(1000.0)
        .field("airControl", &PlayerController::airControl).doc("Fraction of the acceleration while in the air")
            .unitless().min(0.0).max(1.0)
        .field("jumpSpeed", &PlayerController::jumpSpeed).doc("Upward speed of a jump (4.9 m/s: 1.22 m high)").unit("m/s")
            .min(0.0).max(50.0)
        .field("maxSlopeDeg", &PlayerController::maxSlopeDeg).doc("Steepest walkable ground; steeper is a wall").unit("deg")
            .min(0.0).max(89.0)
        .field("stepHeight", &PlayerController::stepHeight).doc("Highest step climbed without jumping").unit("m").min(0.0)
            .max(2.0)
        .field("mass", &PlayerController::mass).doc("Mass pressing on what it stands on").unit("kg").min(1.0).max(10000.0)
        .field("pushForce", &PlayerController::pushForce).doc("The most force it pushes a dynamic body with").unit("N")
            .min(0.0).max(100000.0)
        .field("lookDegPerUnit", &PlayerController::lookDegPerUnit)
            .doc("Turn per unit of mouse movement, or of a look axis's value in a step").unit("deg").min(0.0).max(10.0)
        .field("moveForward", &PlayerController::moveForward).doc("Axis walking along the player's -Z")
        .field("moveRight", &PlayerController::moveRight).doc("Axis walking along the player's +X")
        .field("jump", &PlayerController::jump).doc("Action that jumps when pressed on the ground")
        .field("sprint", &PlayerController::sprint).doc("Action that, while held, walks at sprintSpeed")
        .field("lookX", &PlayerController::lookX).doc("Axis turning the player (yaw), added to the mouse's x")
        .field("lookY", &PlayerController::lookY).doc("Axis tilting the view (pitch), added to the mouse's y")
        .field("spawn", &PlayerController::spawn)
            .doc("The SpawnPoint tag it spawns at: the k-th character with a tag (creation order) takes the k-th "
                 "spawn point with it; empty: it starts where it stands")
            .maxLength(32)
        .field("input", &PlayerController::input)
            .doc("Moved by the world's input (a player); false: by its MoveIntent (an AI or an agent)")
        .field("stream", &PlayerController::stream)
            .doc("Its input stream: one per player; a device's events go to 0, an agent's to the player's it names")
            .unitless().min(0).max(7).omitDefault();
    r.type<MoveIntent>("MoveIntent").component().host(makeComponentOps<MoveIntent>())
        .doc("The move a character without input (PlayerController.input false) makes each step, written by the "
             "enemy AI or an agent. Absent: it stands still, facing as it faces.")
        .field("move", &MoveIntent::move).doc("x right, y forward, in its own frame; each within -1..1").unitless()
            .min(-1.0).max(1.0)
        .field("yawDeg", &MoveIntent::yawDeg).doc("The facing to take, about +Y from -Z").unit("deg").min(-360.0)
            .max(360.0)
        .field("pitchDeg", &MoveIntent::pitchDeg).doc("The look up (positive) or down").unit("deg").min(-89.0).max(89.0)
        .field("jump", &MoveIntent::jump).doc("Jump (when on the ground)")
        .field("sprint", &MoveIntent::sprint).doc("Walk at sprintSpeed");

    r.type<Level>("Level").component().host(makeComponentOps<Level>())
        .doc("A level file (glTF) placed in the world. The level system expands it into one child entity per node of "
             "the file's default scene (LevelNode), with ids hashed from this entity's id and the node's index; those "
             "entities are the file's and are never saved. Node extras give hints (col, body, mass, fric, rest, trig, "
             "surf, vis, spawn, socket, verb) and components by name.")
        .field("asset", &Level::asset).doc("Project-relative path of the .glb or .gltf file, '/' separators")
        .field("lightScale", &Level::lightScale).doc("Multiplier on the file's KHR_lights_punctual intensities")
            .unitless().min(0.0).uiRange(0.0, 10.0)
        .field("collision", &Level::collision).doc("Whether the level's nodes get colliders; false: render only");
    r.type<SpawnPoint>("SpawnPoint").component().host(makeComponentOps<SpawnPoint>())
        .doc("A named place to spawn something, at the entity's world position, facing its -Z. Spawns are taken in "
             "creation order (a level's node order).")
        .field("tag", &SpawnPoint::tag).doc("What spawns here: player, enemy, ...").maxLength(32);
    r.type<Socket>("Socket").component().host(makeComponentOps<Socket>())
        .doc("An attach point at the entity's transform (a level's SOCKET_ empties).")
        .field("name", &Socket::name).doc("The socket's name, as attachments refer to it").maxLength(64);

    r.type<Collider>("Collider").component().host(makeComponentOps<Collider>())
        .doc("The entity collides: the physics system builds a body for it (static unless it has a RigidBody). A zero "
             "size fits the shape to the entity's mesh, in its frame and at its world scale.")
        .field("shape", &Collider::shape).doc("The shape")
        .field("size", &Collider::size)
            .doc("Zero: fit to the mesh. Box: half extents; Sphere: x is the radius; Capsule: x the radius, y the half "
                 "height of its straight part")
            .unit("m").min(0.0)
        .field("offset", &Collider::offset).doc("The shape's centre in the entity's frame, with an explicit size").unit("m")
        .field("trigger", &Collider::trigger)
            .doc("A trigger volume: reports bodies entering and leaving it (trigger_enter, trigger_exit) and blocks "
                 "nothing")
        .field("surface", &Collider::surface)
            .doc("What it is made of, for footsteps on it (concrete, metal, wood, ...); empty is \"default\"")
            .maxLength(32).omitDefault();
    r.type<RigidBody>("RigidBody").component().host(makeComponentOps<RigidBody>())
        .doc("How the entity's body moves. Dynamic bodies are simulated at the world's fixed step and move their "
             "entity; static and kinematic ones follow it.")
        .field("motion", &RigidBody::motion).doc("Static, Dynamic or Kinematic")
        .field("mass", &RigidBody::mass).doc("Mass of a dynamic body").unit("kg").min(0.001)
        .field("friction", &RigidBody::friction).doc("Surface friction").unitless().min(0.0).max(1.0)
        .field("restitution", &RigidBody::restitution).doc("Bounciness: 0 none, 1 elastic").unitless().min(0.0).max(1.0);
    r.type<Joint>("Joint").component().host(makeComponentOps<Joint>())
        .doc("A joint between the body this entity is part of (its own, else its nearest ancestor's) and another's, or "
             "the world. Anchor and axis are in this entity's frame; the other side is the same world point and axis "
             "when the joint is built, except a Distance joint's, which is the other entity's origin. Several joints "
             "on one body: child entities with a Joint each.")
        .field("type", &Joint::type).doc("What the joint lets the bodies do")
        .field("other", &Joint::other).doc("The other body's entity (its own body, else its nearest ancestor's); null: the world")
        .field("anchor", &Joint::anchor).doc("The joint's point in this entity's frame").unit("m")
        .field("axis", &Joint::axis).doc("Hinge axis, slider axis or swing-twist's twist axis, in this entity's frame").unitless()
        .field("limitMin", &Joint::limitMin)
            .doc("Hinge: degrees this entity turns about the axis (right-handed) from the built pose, relative to the "
                 "other; slider: m along the axis; swing-twist: twist degrees; distance: m. With limitMax 0 too: free "
                 "(hinge, slider), the built distance (distance)")
            .unitless().omitDefault()
        .field("limitMax", &Joint::limitMax).doc("The limit's other end, as limitMin").unitless().omitDefault()
        .field("swingDeg", &Joint::swingDeg).doc("Swing-twist: the swing cone's half angle about the axis")
            .unit("deg").angle().min(0.0).max(180.0).omitDefault()
        .field("springHz", &Joint::springHz).doc("Distance: a soft range's frequency; 0 is rigid").unit("Hz").min(0.0).omitDefault()
        .field("springDamping", &Joint::springDamping).doc("Distance: the soft range's damping ratio").unitless().min(0.0).omitDefault()
        .field("breakForce", &Joint::breakForce).doc("The impulse in one step that breaks it; 0 never").unit("N s").min(0.0).omitDefault()
        .field("breakTorque", &Joint::breakTorque).doc("The angular impulse in one step that breaks it; 0 never")
            .unit("N m s").min(0.0).omitDefault();
    r.type<JointState>("JointState").component().host(makeComponentOps<JointState>())
        .doc("The joint broke: its constraint stays built and does nothing. The physics step adds it; a restore from "
             "before the break makes the joint whole.")
        .field("broken", &JointState::broken).doc("The joint is broken; false makes it whole again");

    r.type<AudioMix>("AudioMix").component().singleton().host(makeComponentOps<AudioMix>())
        .doc("The world's sound bank and mix (one per world): an .audio.json declaring the buses and their voice caps, "
             "the sounds (files, bus, priority, distance shape, how long each holds its voice), the ducks, the "
             "snapshots and the reverb presets. Without one, the default bank (master over sfx, music, ambient).")
        .field("asset", &AudioMix::asset).doc("Project-relative path of the .audio.json file, '/' separators");
    r.type<ReverbZone>("ReverbZone").component().host(makeComponentOps<ReverbZone>())
        .doc("A volume whose reverb preset the listener hears while inside it: a box about the entity, in its frame. "
             "Of the zones holding the listener the highest priority wins, then the lowest id; outside them all, the "
             "bank's defaultZone. A volume, not a body: nothing collides with it.")
        .field("preset", &ReverbZone::preset).doc("A reverb preset of the world's .audio.json (room, outdoor, ...)")
            .maxLength(32)
        .field("priority", &ReverbZone::priority).doc("Higher wins where zones overlap").unitless()
        .field("size", &ReverbZone::size).doc("The box's half extents; zero fits it to the entity's mesh").unit("m").min(0.0);

    // ---- effects (sub-project 12) -------------------------------------------------------
    r.type<VfxSet>("VfxSet").component().singleton().host(makeComponentOps<VfxSet>())
        .doc("The world's effect set (one per world): a .vfx.json declaring the effects (emitters of closed-form "
             "particles), decals, tracers and which events' cues spawn them. Without one, the default set.")
        .field("asset", &VfxSet::asset).doc("Project-relative path of the .vfx.json file, '/' separators");
    r.type<VfxEmitter>("VfxEmitter").component().host(makeComponentOps<VfxEmitter>())
        .doc("A placed effect at the entity's pose: while active, rate spawns a second by step count from the step "
             "it became active, or one spawn when it becomes active with rate 0.")
        .field("effect", &VfxEmitter::effect).doc("An effect of the world's .vfx.json").maxLength(64)
        .field("rate", &VfxEmitter::rate).doc("Spawns a second while active; 0: one when it becomes active").unit("Hz")
            .min(0.0).max(1000.0)
        .field("seed", &VfxEmitter::seed).doc("Varies its particles' random draws").unitless()
        .field("active", &VfxEmitter::active).doc("Spawning; turning it on starts the count again");

    // ---- animation (sub-project 10) ----------------------------------------------------
    r.type<SkinnedMesh>("SkinnedMesh").component().host(makeComponentOps<SkinnedMesh>())
        .doc("A skinned mesh: the entity's MeshSource (a ModelAsset with `mesh`) is deformed by a skin of a glTF file, "
             "posed by the entity's Animator. The instance gets its own deformed copy of the mesh.")
        .field("file", &SkinnedMesh::file).doc("The glTF (.glb) holding the skin")
        .field("skin", &SkinnedMesh::skin).doc("The skin's name or \"#<index>\"; empty: the file's first skin")
            .maxLength(256);
    r.type<AnimLayerState>("AnimLayerState")
        .doc("One layer of an Animator: its state, the steps in it, and the fade from the state it left.")
        .field("state", &AnimLayerState::state).doc("The layer's current state (empty: its initial state)").maxLength(256)
        .field("steps", &AnimLayerState::steps).doc("Simulation steps since entering it").unitless()
        .field("from", &AnimLayerState::from).doc("The state faded from; empty when not fading").maxLength(256)
        .field("fromSteps", &AnimLayerState::fromSteps).doc("Its steps").unitless()
        .field("fade", &AnimLayerState::fade).doc("The fade's length in steps (0: none)").unitless()
        .field("faded", &AnimLayerState::faded).doc("Steps of the fade done").unitless();
    r.type<Animator>("Animator").component().host(makeComponentOps<Animator>())
        .doc("Poses an entity by an animation graph (.animgraph.json): each layer's state machine advances one step a "
             "simulation step (integers only, so a snapshot holds it exactly). The pose is presentation: never saved or "
             "hashed.")
        .field("graph", &Animator::graph).doc("The animation graph file")
        .field("layers", &Animator::layers).doc("One a graph layer, in order; empty: filled at the first step");
    r.type<JointAttach>("JointAttach").component().host(makeComponentOps<JointAttach>())
        .doc("Drawn at a joint of its parent's pose (times its own Transform): a rifle in a hand. Presentation: its "
             "Transform, and what the step sees, do not follow the joint.")
        .field("joint", &JointAttach::joint).doc("The joint's name in the parent's skeleton").maxLength(256);

    // ---- runtime state (catalogue only, never saved) -----------------------------------

    r.type<MeshHandle>("MeshHandle").runtime()
        .doc("A mesh slot in the GPU render scene. Runtime only: files refer to meshes by source.")
        .field("index", &MeshHandle::index).doc("Slot index; 4294967295 means none").unitless()
        .field("generation", &MeshHandle::generation).doc("Slot generation; a stale handle no longer matches").unitless();
    r.type<MaterialHandle>("MaterialHandle").runtime()
        .doc("A material slot in the GPU render scene. Runtime only.")
        .field("index", &MaterialHandle::index).doc("Slot index; 4294967295 means none").unitless()
        .field("generation", &MaterialHandle::generation).doc("Slot generation").unitless();

    r.type<Children>("Children").component().runtime().host(makeComponentOps<Children>())
        .doc("The entity's children in attach order. Derived: rebuilt from Parent.")
        .field("entities", &Children::entities).doc("Child entities");
    r.type<WorldTransform>("WorldTransform").component().runtime().host(makeComponentOps<WorldTransform>())
        .doc("World matrices computed by the transform system from Transform and Parent.")
        .field("world", &WorldTransform::world).doc("Object-to-world matrix this frame, column-major").unitless()
        .field("prevWorld", &WorldTransform::prevWorld).doc("Last frame's object-to-world matrix (motion vectors)").unitless();
    r.type<MeshRenderer>("MeshRenderer").component().runtime().host(makeComponentOps<MeshRenderer>())
        .doc("The GPU mesh and materials an entity renders with. Built from the entity's mesh source.")
        .field("mesh", &MeshRenderer::mesh).doc("Mesh slot")
        .field("materials", &MeshRenderer::materials).doc("One material per submesh, in submesh order")
        .field("visible", &MeshRenderer::visible).doc("Whether the entity is drawn");
    r.type<RenderSlot>("RenderSlot").component().runtime().host(makeComponentOps<RenderSlot>())
        .doc("The entity's instance slot in the GPU render scene. Owned by the render extract.")
        .field("slot", &RenderSlot::slot).doc("Instance slot index").unitless();
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
    r.type<InstantiateDirty>("InstantiateDirty").component().runtime()
        .doc("Tag: MeshSource or Material changed; instantiate rebuilds the mesh renderer this frame.");
    r.type<InstantiateError>("InstantiateError").component().runtime().host(makeComponentOps<InstantiateError>())
        .doc("Why instantiate could not build the entity's mesh. The entity then has no MeshRenderer.")
        .field("code", &InstantiateError::code).doc("E_ASSET_NOT_FOUND, E_ASSET_INVALID or E_MESH_INVALID")
        .field("message", &InstantiateError::message).doc("What went wrong, for people and agents");
    r.type<LevelNode>("LevelNode").component().runtime().host(makeComponentOps<LevelNode>())
        .doc("This entity is a node of a level: made by the level system from the file, never saved, rebuilt from "
             "the file when its Level changes.")
        .field("level", &LevelNode::level).doc("The entity with the Level")
        .field("index", &LevelNode::index).doc("The node's index in the default scene, depth first").unitless();
    r.type<LevelDirty>("LevelDirty").component().runtime()
        .doc("Tag: the Level changed; the level system re-expands it this frame.");
    r.type<ColliderPart>("ColliderPart").component().runtime().host(makeComponentOps<ColliderPart>())
        .doc("This entity's collider is part of another entity's body (a level's UCX_ hulls): one compound body.")
        .field("body", &ColliderPart::body).doc("The entity whose body it is part of");
    r.type<CollisionMesh>("CollisionMesh").component().runtime().host(makeComponentOps<CollisionMesh>())
        .doc("The mesh a level node's collider is fitted to when the node is not drawn (vis 0, UCX_ hulls) and so has "
             "no MeshSource. Set by the level system.")
        .field("source", &CollisionMesh::source).doc("The glTF mesh");
    r.type<PhysicsBody>("PhysicsBody").component().runtime().host(makeComponentOps<PhysicsBody>())
        .doc("The entity's body in the physics backend. Owned by the physics system.")
        .field("handle", &PhysicsBody::handle).doc("Body handle; 4294967295 means none").unitless();
    r.type<PhysicsMoved>("PhysicsMoved").component().runtime()
        .doc("Tag: an entity with a body moved; the physics system moves the body this frame.");
    r.type<PlayerState>("PlayerState").component().runtime().host(makeComponentOps<PlayerState>())
        .doc("The player as the physics system left it after the last step. Owned by the physics system.")
        .field("velocity", &PlayerState::velocity).doc("World velocity").unit("m/s")
        .field("onGround", &PlayerState::onGround).doc("Standing on walkable ground")
        .field("yawDeg", &PlayerState::yawDeg).doc("Facing about +Y from -Z, counter-clockwise seen from above").unit("deg")
        .field("pitchDeg", &PlayerState::pitchDeg).doc("Looking up (positive) or down, within 89 degrees").unit("deg")
        .field("character", &PlayerState::character).doc("Character handle; 4294967295 means none").unitless();
    r.type<BodyMotion>("BodyMotion").component().runtime().host(makeComponentOps<BodyMotion>())
        .doc("The motion a dynamic body or a player carries into the next step. Written by the physics system; "
             "never saved or hashed, but held by snapshots (a restore whose bodies were rebuilt since puts it back).")
        .field("linear", &BodyMotion::linear).doc("Linear velocity, world (a player's velocity)").unit("m/s")
        .field("angular", &BodyMotion::angular).doc("Angular velocity, world").unit("rad/s")
        .field("asleep", &BodyMotion::asleep).doc("The body sleeps; its velocities are zero");
    registerEventTypes(r);
}

} // namespace ohao::ecs
