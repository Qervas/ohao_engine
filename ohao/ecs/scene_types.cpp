#include "ecs/scene_types.hpp"
#include "ecs/components.hpp"
#include "ecs/render_extract.hpp"

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
        .value("Dynamic", Motion::Dynamic, "Moved by the simulation (not stepped yet: sub-project 6)")
        .value("Kinematic", Motion::Kinematic, "Moved by its transform, pushing dynamic bodies");

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
    r.type<MaterialOverride>("MaterialOverride").component().host(makeComponentOps<MaterialOverride>())
        .doc("The material of a mesh without its own materials (primitives). For a model with materials it only "
             "sets the legacy RT GI albedo.")
        .field("baseColor", &MaterialOverride::baseColor).doc("Albedo").unit("linear-rgb").color().min(0.0).max(1.0)
        .field("roughness", &MaterialOverride::roughness).doc("GGX roughness").unitless().min(0.0).max(1.0)
        .field("metallic", &MaterialOverride::metallic).doc("0 = dielectric, 1 = metal").unitless().min(0.0).max(1.0)
        .field("emissiveStrength", &MaterialOverride::emissiveStrength).doc("Emission multiplier").unitless().min(0.0)
        .field("subsurface", &MaterialOverride::subsurface)
            .doc("Subsurface scattering (skin, wax): how much of the lit colour the screen-space blur spreads under "
                 "the surface; 0 = none. Ignored where emissiveStrength > 0.")
            .unitless().min(0.0).max(1.0);

    r.type<Camera>("Camera").component().singleton().host(makeComponentOps<Camera>())
        .doc("The scene camera (one per world). Its pose is the entity's world transform: it looks along -Z with +Y "
             "up; roll is ignored.")
        .field("fovDeg", &Camera::fovDeg).doc("Vertical field of view").unit("deg").angle().min(1.0).max(179.0)
        .field("nearPlane", &Camera::nearPlane).doc("Near clip distance (raster passes)").unit("m").min(0.0001)
        .field("farPlane", &Camera::farPlane).doc("Far clip distance (raster passes); keep it above nearPlane")
            .unit("m").min(0.001)
        .field("active", &Camera::active).doc("false: the host keeps its own camera");
    r.type<Environment>("Environment").component().singleton().host(makeComponentOps<Environment>())
        .doc("Image-based lighting (one per world): an equirectangular HDR image lighting and backing the scene.")
        .field("hdri", &Environment::hdri).doc("Project-relative path of the .hdr image; null for none")
        .field("intensity", &Environment::intensity).doc("Multiplier on the image's radiance").unitless().min(0.0)
            .uiRange(0.0, 10.0);

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

    r.type<Level>("Level").component().host(makeComponentOps<Level>())
        .doc("A level file (glTF) placed in the world. The level system expands it into one child entity per node of "
             "the file's default scene (LevelNode), with ids hashed from this entity's id and the node's index; those "
             "entities are the file's and are never saved. Node extras give hints (col, body, mass, fric, rest, vis, "
             "spawn, socket) and components by name.")
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
        .field("offset", &Collider::offset).doc("The shape's centre in the entity's frame, with an explicit size").unit("m");
    r.type<RigidBody>("RigidBody").component().host(makeComponentOps<RigidBody>())
        .doc("How the entity's body moves. Dynamic bodies are built but not stepped until the simulation steps "
             "physics (sub-project 6).")
        .field("motion", &RigidBody::motion).doc("Static, Dynamic or Kinematic")
        .field("mass", &RigidBody::mass).doc("Mass of a dynamic body").unit("kg").min(0.001)
        .field("friction", &RigidBody::friction).doc("Surface friction").unitless().min(0.0).max(1.0)
        .field("restitution", &RigidBody::restitution).doc("Bounciness: 0 none, 1 elastic").unitless().min(0.0).max(1.0);

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
    r.type<EmissiveProxyLight>("EmissiveProxyLight").component().runtime().host(makeComponentOps<EmissiveProxyLight>())
        .doc("A point light approximating a mesh's emissive texture. Derived from the model.")
        .field("localCenter", &EmissiveProxyLight::localCenter).doc("Center in the entity's model space").unit("m")
        .field("radius", &EmissiveProxyLight::radius).doc("Emitter radius").unit("m")
        .field("color", &EmissiveProxyLight::color).doc("Emitted color").unit("linear-rgb").color()
        .field("intensity", &EmissiveProxyLight::intensity).doc("Brightness multiplier").unitless();
    r.type<GiMaterialLegacy>("GiMaterialLegacy").component().runtime().host(makeComponentOps<GiMaterialLegacy>())
        .doc("LEGACY: the RT GI per-instance material word (albedo rgb, packed roughness/metallic in a).")
        .field("packed", &GiMaterialLegacy::packed).doc("rgb = albedo, a = packed roughness/metallic/vertex-count rule").unitless();
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
        .doc("Tag: MeshSource or MaterialOverride changed; instantiate rebuilds the mesh renderer this frame.");
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
}

} // namespace ohao::ecs
