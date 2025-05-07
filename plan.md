3. Migration/Refactor Plan

1. **Design new Scene/SceneNode/SceneObject classes** (with clear ownership and hierarchy).
2. **Implement ECS or hybrid Actor/Component system** for flexibility.
3. **Refactor rendering to use per-object transforms/materials** and a central buffer manager.
4. **Integrate physics as a component/system**, decoupled from rendering.
5. **Implement serialization** for scenes and objects.
6. **Gradually migrate existing code** to use the new architecture, starting with simple objects and expanding.

Go with Actor/Component (Unreal-style), but design your components and systems to be as data-oriented as possible.**
This gives you:
- Fast iteration and prototyping.
- A clear path to editor tooling and visual scripting.
- The ability to add ECS-like systems later if you need more performance.

plan:
Absolutely! Here’s a comprehensive, actionable migration/refactor plan to move your engine from its current state to a robust Unreal-style Actor/Component architecture, with a focus on scalability, maintainability, and visual impact.

---

# OHAO Engine Migration/Refactor Plan

## **Phase 1: Planning & Preparation**

### 1.1. **Define Goals**
- Support robust multi-object management (hierarchy, transforms, components).
- Enable flexible rendering and physics integration.
- Prepare for editor tooling and serialization.
- Maintain/improve performance and code clarity.

### 1.2. **Audit Existing Code**
- Identify all code that manages scene objects, transforms, rendering, and physics.
- List all current object types (meshes, lights, cameras, etc.).
- Note all places where object management is tightly coupled or hard to extend.

---

## **Phase 2: Core Architecture Implementation**

### 2.1. **Design New Core Classes**
- `Scene`: Manages all objects, root node, lookup, and serialization.
- `SceneObject`: Base class for all objects in the scene (supports hierarchy).
- `Component`: Base class for modular data/logic.
- Example components: `TransformComponent`, `MeshComponent`, `MaterialComponent`, `LightComponent`, `PhysicsComponent`.

### 2.2. **Implement SceneObject & Component System**
- Implement `SceneObject` with:
    - Name, parent/child hierarchy, component management.
    - Methods for adding/removing components and children.
- Implement `Component` base with:
    - Owner pointer, virtual lifecycle methods (`onAttach`, `onDetach`, `onUpdate`).
- Implement `TransformComponent` (always present).
- Implement basic `MeshComponent`, `MaterialComponent`, etc.

### 2.3. **Refactor Scene Management**
- Refactor `Scene` to own all top-level objects and provide APIs for:
    - Adding/removing objects.
    - Traversing the scene graph.
    - Looking up objects by name/ID.

---

## **Phase 3: Incremental Migration**

### 3.1. **Migrate Object Creation**
- Replace old object creation with `SceneObject` + components.
- Update all code that creates/owns objects to use the new system.

### 3.2. **Migrate Rendering**
- Refactor rendering code to:
    - Traverse the new scene graph.
    - Use `TransformComponent` for per-object transforms.
    - Use `MeshComponent` and `MaterialComponent` for rendering data.
- Update buffer management to support multiple objects (combine vertex/index data, use per-object transforms/materials).

### 3.3. **Migrate Physics**
- Refactor physics integration to use `PhysicsComponent`.
- Physics system updates `TransformComponent` (decoupled from rendering).

### 3.4. **Migrate Serialization**
- Implement serialization for `Scene`, `SceneObject`, and components (JSON or binary).
- Update save/load code to use new architecture.

### 3.5. **Migrate Editor/Tooling**
- Update editor panels (outliner, properties, etc.) to use new scene graph and component system.
- Ensure selection, manipulation, and property editing work with new objects/components.

---

## **Phase 4: Cleanup & Optimization**

### 4.1. **Remove Old Code**
- Remove legacy object management, rendering, and physics code.
- Remove any tightly-coupled or redundant systems.

### 4.2. **Optimize Hot Paths**
- Profile scene traversal, rendering, and physics.
- Optimize buffer updates, batching, and per-frame allocations.
- Consider ECS-style data layouts for hot systems if needed.

### 4.3. **Polish API & Documentation**
- Document new architecture and usage patterns.
- Write example code and migration guides for contributors.

---

## **Phase 5: Advanced Features & Visuals**

### 5.1. **Advanced Rendering**
- Add support for instancing, batching, and advanced material systems.
- Implement per-object UBOs/push constants for efficient rendering.

### 5.2. **Physics/Gameplay Extensions**
- Add more component types (colliders, scripts, audio, etc.).
- Implement event system for component communication.

### 5.3. **Editor Enhancements**
- Improve visual tools for hierarchy, components, and properties.
- Add drag-and-drop, duplication, and undo/redo.

---

## **Milestones & Deliverables**

1. **Core system implemented** (Scene, SceneObject, Component, Transform).
2. **Basic multi-object rendering** working with new architecture.
3. **Physics and serialization** migrated.
4. **Editor/tools** updated.
5. **Old code removed, new system documented.**

---

## **Tips for Success**

- **Migrate incrementally:** Keep the engine running at each step.
- **Write tests/examples** for each new system.
- **Document as you go**—future you (and collaborators) will thank you.
- **Profile and optimize** only after correctness and clarity.
