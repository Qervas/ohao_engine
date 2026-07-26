---
module: scene
id: components
title: Components
standard: v2
---

## The type map is the whole design

An `Actor` stores its components twice over: a `std::vector<std::shared_ptr<Component>>` that owns them in insertion order, and an `unordered_map<std::type_index, shared_ptr<Component>>` that answers every lookup. Nothing walks the vector on a hot path — the GBuffer pass, the RT builder and both light uploads all begin with a hashed `getComponent<T>()`. The map is keyed on `typeid(T)`: the *static* template argument at the call site, not the object's dynamic type.

{{cite ohao/scene/actor/actor.hpp "componentsByType[std::type_index(typeid(T))] = component;"}}

Two consequences follow, both sharp. There is exactly one component per type per actor: `addComponent<MeshComponent>()` twice appends both to the vector but leaves only the second in the map, and a later `removeComponent<MeshComponent>()` unlinks that second one — the first stays alive, owned, and invisible to every renderer. And subclassing would not survive the map: a component added as a derived type keys under *that* `type_index`, so `getComponent<MeshComponent>()` returns null and the actor silently never draws. That hazard is latent rather than live only because the hierarchy is flat — transform, mesh, material, light and physics all derive straight from `Component`, and nothing in the tree derives from them. The free helpers next to `Component` bake the same assumption in: `componentCast<T>` is a `typeid` equality test, not a `dynamic_cast`.

{{cite ohao/scene/component/component.hpp "typeid(*component) == typeid(T)"}}

:::why
The rejected alternative is registering each component under every base in its hierarchy, or scanning the vector with `dynamic_cast`. Either makes `getComponent<MeshComponent>()` polymorphic at the cost of a linear scan plus RTTI walk per actor per pass — and the deferred path already pays three `getComponent` calls per actor per pass (mesh, transform, material). OHAO buys the O(1) lookup and pays with a rule you have to know.
:::

## Transform is added for you, and its world matrix is lazy

`Actor`'s constructor adds a `TransformComponent` before anything else, so every actor is born with one.

{{cite ohao/scene/actor/actor.cpp "addComponent<TransformComponent>();"}}

That is a convention, not an enforcement. `removeComponent<T>()` is public and carries no exemption for the transform, and `removeAllComponents()` empties the owning vector and the type map outright; after either, `getTransform()` returns null. Nothing in the tree does that today, but the engine does not trust the invariant either — `Actor::getWorldMatrix()` and the GBuffer pass both null-check the transform before dereferencing it, while `uploadLightBuffer` does not.

{{cite ohao/scene/actor/actor.cpp "componentsByType.clear();"}}

The component stores local TRS plus two dirty flags. `setDirty()` marks itself and recurses into every child transform, so moving a parent invalidates a whole subtree in one call. `getWorldMatrix()` is declared `const` and then `const_cast`s itself to rebuild on demand — a read that mutates, which is why transforms must not be sampled from two threads at once.

{{cite ohao/scene/component/transform_component.cpp "const_cast<TransformComponent*>(this)->updateWorldMatrix();"}}

Two details a cleanup pass would get wrong. `clearDirty()` clears only the local flag and deliberately leaves `worldDirty` set, with the reason in the source — an external "I consumed this" acknowledgement must not be able to freeze a stale world matrix.

{{cite ohao/scene/component/transform_component.cpp "// Note: We don't clear worldDirty here as it might still need updating"}}

And `getLocalMatrix()` has no dirty check at all: `updateLocalMatrix()` is private and reached only through `updateWorldMatrix()`, so the local matrix stays identity until somebody asks for the world matrix. It has no callers outside the class, which is the only reason this is latent rather than a bug. Nothing in `setParent()` rejects making a transform its own ancestor either, and `setDirty()`'s recursion has no visit guard.

## Registering a mesh with the scene, twice

`Scene` keeps two component registries, one of raw `MeshComponent*` and one of raw `PhysicsComponent*`, both filled from `Actor::onComponentAdded` and both dedup-guarded by a linear `std::find`.

{{cite ohao/scene/scene.hpp "std::vector<PhysicsComponent*> physicsComponents;"}}

Mesh is the one that arrives twice. Its own `initialize()` walks owner → scene and registers:

{{cite ohao/scene/component/mesh_component.cpp "scene->onMeshComponentAdded(this);"}}

while `Actor::onComponentAdded` separately `dynamic_pointer_cast`s the fresh component and registers it again:

{{cite ohao/scene/actor/actor.cpp "scene->onMeshComponentAdded(meshComponent.get());"}}

Both fire when a mesh joins an actor that is already in a scene, because `addComponent` calls `initialize()` on an active actor and then invokes the hook. The `std::find` inside `Scene::onMeshComponentAdded` is what stops the registry growing a duplicate per mesh — though nothing downstream would notice if it did. The vector is never iterated — its accessor `getMeshComponents()` has no callers and `Scene::destroy` only clears it; removal goes through `std::erase`, which would take both copies anyway; and the sole effect of a redundant insert is re-raising the already-set `needsBufferUpdate`.

{{cite ohao/scene/scene.cpp "if (std::find(meshComponents.begin(), meshComponents.end(), component) == meshComponents.end()) {"}}

The physics registry has the same guard around more work. `Scene::onPhysicsComponentAdded` also hands the component the physics world and re-runs `initialize()`, and those two calls sit *outside* the guard — which is what makes physics work at all. The `initialize()` that `addComponent` fires finds `m_physicsWorld` still null and creates nothing; this second one is where the Jolt body is built.

{{cite ohao/scene/scene.cpp "component->setPhysicsWorld(physicsWorld.get());"}}

Swapping the model on an existing component takes a third route: `setModel` raises the scene's buffer-dirty flag directly, because a new model changes the packed vertex and index totals the renderer allocated for.

{{cite ohao/scene/component/mesh_component.cpp "scene->onMeshComponentChanged(this);"}}

## The buffer offsets on MeshComponent are not the ones the GPU uses

`MeshComponent` carries a `MeshBufferInfo` — vertex offset, index offset, index count, vertex count — and exposes setters for the renderer to fill in. No renderer does. `VulkanRenderer` builds its own map keyed by actor ID while packing the combined buffers:

{{cite ohao/gpu/vulkan/scene_upload.cpp "m_meshBufferMap[actor->getID()] = MeshBufferInfo{"}}

and both the deferred draw loop and `renderSceneObjects` look up *that* map. The component's copy is written in exactly two places: `setModel`, which zeroes both offsets and refills the two counts from the model, and `setBufferInfo`, whose only caller is `tests/engine/engine_tests.cpp`. `setBufferOffsets` — the setter whose signature reads like the renderer's entry point — has no callers anywhere, and neither do the three narrow accessors beside it (`getVertexOffset`, `getIndexOffset`, `getBufferIndexCount`).

{{cite ohao/scene/component/mesh_component.cpp "void MeshComponent::setBufferOffsets("}}

The same holds one level up: `Scene` maintains a `meshComponents` vector on every add and remove, but its `getMeshComponents()` accessor has no callers in the tree — the renderers iterate `getAllActors()` and hash-lookup per actor instead. Neither store is harmful; neither is a source of truth. Reading the component's offsets to decide what to draw gives you zeros.

## LightComponent is a shader ABI wearing a class

`LightType`'s enumerator values are not an internal detail. The RT upload casts the enum straight into the GPU struct's type channel, and `GPULight`'s header comment restates the same numbering as the shader contract.

{{cite ohao/scene/component/light_component.hpp "Sphere = 0,       // point light with radius (soft shadows)"}}
{{cite ohao/gpu/vulkan/light_upload.cpp "gl.positionAndType = glm::vec4(pos, static_cast<float>(lc->getLightType()));"}}

Renumbering the enum therefore re-lights every path-traced frame with no compiler complaint. The deferred pipeline does *not* share that numbering: `deferred_lighting.frag` treats type 0 as directional and type 1 as point, the reverse of `LightType`, so `VulkanRenderer::updateLightBuffer` renumbers on the way out.

{{cite shaders/core/deferred_lighting.frag "            L = normalize(-light.direction.xyz);"}}

It renumbers twice, because the frame-indexed overload is a copy of the unindexed one — and the copy has already drifted. Both end with a fallback that inserts a default light when the scene has none, and the two fallbacks disagree: the original emits a warm tint at intensity `glm::pi<float>()`, with a comment naming that as compensation for the energy-conserving divide-by-pi in the diffuse BRDF; the copy emits pure white at intensity 1.

{{cite ohao/gpu/vulkan/buffer_setup.cpp "defaultLight.color = glm::vec4(1.0f, 0.98f, 0.95f, glm::pi<float>());"}}
{{cite ohao/gpu/vulkan/buffer_setup.cpp "defaultLight.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);"}}

Both overloads are live — `renderDeferred` and `renderLegacy` take the unindexed one, `renderMultiFrame` the frame-indexed one — so an empty scene gets a different key light, in tint and by a factor of pi in intensity, depending on which entry point ran.

The scalar fields are overloaded per type as well. `dirAndParam.w` is `LightComponent::radius` for every type *except* spot, which overrides it with the inner cone angle and displaces the outer angle into `extra.w`; area rectangles put their two edge vectors and a CPU-precomputed `length(cross(e1, e2))` into `extra`/`extra2`.

{{cite ohao/gpu/vulkan/light_upload.cpp "float dirParam = lc->getRadius();"}}

Angles travel in degrees; both consumers convert (`cos(radians(...))` in the raygen, `glm::cos(glm::radians(...))` in the deferred packer). The same component set also meets two different ceilings: the deferred UBO is a fixed array of 8, while the RT storage buffer is sized from the vector at upload time.

{{cite ohao/gpu/vulkan/renderer.hpp "constexpr uint32_t MAX_LIGHTS = 8;"}}

One honest gap. Both uploads read the actor's *local* position rather than its world position, while the mesh path uses the full world matrix — parent a light under a moving actor and the mesh follows while the light stays behind. The deferred variable is even named `worldPos`.

{{cite ohao/gpu/vulkan/light_upload.cpp "auto pos = actor->getTransform()->getPosition();"}}
{{cite ohao/render/deferred/gbuffer_pass.cpp "glm::mat4 modelMatrix = transformComp ? transformComp->getWorldMatrix() : glm::mat4(1.0f);"}}

## AreaRect is half a light

`LightType::AreaRect` has the geometry and not the radiometry. The path tracer's NEE block samples the rectangle uniformly in the edge basis — `lightCenter + edge1 * u + edge2 * v` with `u, v` in the unit square, so the uploaded position is really a *corner* — takes the normal from `cross(edge1, edge2)`, and weights by cosine times the uploaded area over squared distance. All of that is right.

{{cite shaders/rt/pt_raygen.rgen "// ==== Analytic direct NEE at bounce 0 ===="}}

The radiance that weight multiplies is not. `Le` is computed once, above the type branch, as intensity divided by the area of a *sphere* of radius `dirAndParam.w` — and for an `AreaRect`, `dirAndParam.w` is `LightComponent::radius`, a field the area path never touches. So a rectangle light is divided by `4·pi·r²` of an unrelated radius (0.5 unless someone calls `setRadius`, giving a divisor of pi) no matter what its edges are: `setAreaEdges` changes the emitter's shape and its geometric falloff and leaves its brightness alone. That block is pasted three times in `pt_raygen.rgen` — primary hit plus both bounce loops, differing only in the hit-point variable — so the same divisor applies at every depth.

The deferred pipeline does not represent area lights at all. Its type remap is a two-branch ternary: `Sphere` → 1, `Directional` → 0, *everything else* → 2. `AreaRect` (3) therefore lands in the UBO as a spot light and is shaded through `calculateSpotAttenuation` against a cone it does not have, using whatever `innerConeAngle`/`outerConeAngle` the constructor happened to leave behind.

{{cite shaders/core/deferred_lighting.frag "attenuation *= calculateSpotAttenuation(L, normalize(light.direction.xyz),"}}

`PrimitiveType` also stops at `SpotLight`, so an area light exists at all only if you call `setLightType` by hand. Exactly one caller in the tree does: `examples/turntable.cpp`, in its Cornell mode.

{{cite ohao/scene/component/component_factory.hpp "SpotLight"}}

## The defaults in the header are not the defaults you get

`LightComponent` declares default member initialisers and then re-initialises a subset of them in its constructor's member-init list. The constructor wins: a fresh `LightComponent` has intensity 1.0 and range 10.0, not the 10.0 and 50.0 the header advertises — while `radius = 0.5f` and the area edge vectors, absent from the init list, do come from the header.

{{cite ohao/scene/component/light_component.hpp "float lightIntensity = 10.0f;"}}
{{cite ohao/scene/component/light_component.cpp "    , lightIntensity(1.0f)"}}

The intensity half of that divergence is currently unobservable: `ComponentFactory::setupLightComponent` always calls `setIntensity` from its `ComponentSet`, and every hand-constructed `LightComponent` in the tree — the three examples, `SceneFramer`, `inverse/scene_builder.hpp` — sets intensity explicitly too. Nothing ever reads either default.

The range half is the one that can bite, because no hand-built light calls `setRange`. Those lights carry the constructor's 10.0, not the header's 50.0, and the deferred attenuation windows on that value with a fourth-power cutoff — so a hand-authored point light goes fully black past ten units in the deferred pipeline while the RT path, which never uploads `range` at all, keeps lighting.

{{cite shaders/core/deferred_lighting.frag "float windowing = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);"}}

## Packs, factories, and the API that never shipped

`ComponentPack` is a variadic fold over `addComponent`, guarded by `hasComponent<T>()` so that applying two overlapping packs cannot double-add and clobber the type map — the aliasing failure described at the top of this page.

{{cite ohao/scene/component/component_pack.hpp "(addComponent<Components>(actor), ...);"}}

`ComponentFactory` maps a `PrimitiveType` to a `ComponentSet` in a switch, then configures each component from it. Lights get `LightOnlyPack` and are deliberately mesh-less and material-less; every other type — `Cube`, `Sphere`, `Platform`, `Cylinder`, `Cone` and `Empty` — falls into the else branch, which applies `StandardObjectPack` and then calls `setupPhysicsComponent` unconditionally. `config.needsMesh` and `config.needsMaterial` gate their setup calls two lines later; `config.needsPhysics` gates nothing. Its only reader anywhere is `ComponentManager::validateComponentSetup`, which has no callers.

{{cite ohao/scene/component/component_factory.cpp "LightOnlyPack::applyTo(actor);"}}
{{cite ohao/scene/component/component_factory.cpp "setupPhysicsComponent(physicsComponent.get(), config, type);"}}

For the five solids that is what the switch asked for anyway. For `Empty` it is the opposite of what the switch asked for: `getComponentSet` leaves every flag false under the comment "Empty object - only has transform", and the actor still ends up with a mesh component, a material component, a physics component, and — through the `default:` arm of `setupPhysicsShape` — a 1 kg dynamic box collider of half-extent 0.5. Latent rather than live: no caller in the tree passes `Empty`, so the `default:` arm is reached only from that enumerator.

{{cite ohao/scene/component/component_factory.cpp "// Empty object - only has transform"}}

The designated-initialiser helpers on `ComponentSet` — `visualOnly()`, `physicsObject()`, `staticCollider()` — have no callers; `getComponentSet()` hand-builds equivalent field sets per case instead. The same is true of `LightComponent`'s `applyDirectionalSun` and `applySphereKey` presets. The sun preset agrees with the factory's `DirectionalLight` case on intensity 3.0 and differs only in tint and default direction; the sphere preset defaults to intensity 10.0 against the factory's 1.0 for `PointLight`. Two descriptions of "the default sphere light" that disagree by a factor of ten is the drift an uncalled API accumulates.

{{cite ohao/scene/component/component_factory.hpp "[[nodiscard]] static constexpr ComponentSet staticCollider() {"}}
{{cite ohao/scene/component/light_component.hpp "float intensity = 10.0f,"}}
{{cite ohao/scene/component/component_factory.cpp "config.intensity = 1.0f;"}}

:::key
A component is visible only as the exact type it was added as, and only one instance of that type survives per actor. Every renderer entry point is a `getComponent<T>()` on that map — what the map cannot see does not exist.
:::

## Contracts

- `addComponent<T>()` on a type the actor already has silently orphans the previous instance: it stays in the owning vector, drops out of the type map, and no renderer finds it again.
- The constructor's `TransformComponent` is a convention, not a protected slot: `removeAllComponents()` and `removeComponent<TransformComponent>()` both clear it, after which `uploadLightBuffer` dereferences a null transform.
- Transforms must be sampled from one thread: `getWorldMatrix()` is `const` but `const_cast`s to rebuild lazily.
- `getLocalMatrix()` is stale unless `getWorldMatrix()` was called since the last `setDirty()`; only the world accessor triggers a rebuild.
- `LightType`'s numeric values are the path tracer's wire format — reordering the enum changes shading with no build error. The deferred UBO uses a different numbering and remaps at upload, in two hand-maintained copies whose zero-light fallbacks have already diverged by a factor of pi.
- An `AreaRect` light's radiance ignores its rectangle: the raygen normalises every non-spot light by the sphere area implied by `LightComponent::radius`. Resizing the edges changes falloff, not brightness.
- `AreaRect` has no deferred branch at all — the type remap collapses it onto the spot path, which then applies a cone attenuation from angles nobody set.
- Lights are positioned from the actor's local transform, meshes from the world matrix, so parented lights are placed wrong in both pipelines.
- `ComponentFactory` calls `setupPhysicsComponent` on every non-light primitive without consulting `config.needsPhysics` — the one flag in `ComponentSet` that gates nothing — so `PrimitiveType::Empty` would receive a dynamic box collider its own `ComponentSet` says it should not have.
- `Component::componentID` comes from a plain non-atomic static counter, unlike `SceneObject`'s `std::atomic` object ID, so components must be constructed on one thread. Nothing reads `componentID` or `Component::guid` today.
