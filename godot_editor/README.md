# OHAO Engine - Godot Editor Integration

Use Godot 4.3+ as the editor UI while using OHAO's Vulkan renderer and physics engine.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Godot Editor                          │
│  ┌──────────┐  ┌─────────────────────┐  ┌────────────┐  │
│  │Scene Tree│  │      Viewport       │  │ Inspector  │  │
│  │          │  │                     │  │            │  │
│  │ - Cube   │  │  OHAO C++ Engine    │  │ Position   │  │
│  │ - Sphere │  │  (GDExtension)      │  │ Physics    │  │
│  │ - Ground │  │                     │  │ Material   │  │
│  └──────────┘  │  Vulkan Renderer    │  └────────────┘  │
│                │  Physics Engine     │                   │
│                └─────────────────────┘                   │
└─────────────────────────────────────────────────────────┘
```

## Prerequisites

1. **Godot 4.3+** - Download from https://godotengine.org/download
2. **SCons** - `pip install scons`
3. **OHAO Engine built** - Run `cmake --build build` in parent directory

## Building the GDExtension

```bash
# From godot_editor directory
cd godot_editor

# Build godot-cpp bindings first
cd godot-cpp
scons platform=macos  # or linux, windows
cd ..

# Build OHAO extension
scons platform=macos target=template_debug
```

## Project Structure

```
godot_editor/
├── godot-cpp/          # Godot C++ bindings (git submodule)
├── src/                # GDExtension source files
│   ├── register_types.cpp
│   ├── ohao_viewport.cpp/.h
│   └── ohao_physics_body.cpp/.h
├── project/            # Godot project
│   ├── project.godot
│   ├── bin/
│   │   └── ohao.gdextension
│   ├── scenes/
│   └── scripts/
└── SConstruct          # Build script
```

## Custom Nodes

### OhaoViewport
Replaces Godot's rendering with OHAO's Vulkan pipeline.

```gdscript
var viewport = OhaoViewport.new()
viewport.initialize_renderer()
viewport.sync_scene()  # Sync Godot scene to OHAO
```

### OhaoPhysicsBody
Physics body using OHAO's custom physics engine.

```gdscript
var body = OhaoPhysicsBody.new()
body.body_type = OhaoPhysicsBody.BODY_DYNAMIC
body.mass = 1.0
body.shape_type = OhaoPhysicsBody.SHAPE_BOX
body.apply_force(Vector3(0, -9.81, 0))
```

## Development Workflow

1. **Scene editing** - Use Godot's scene tree, inspector, etc.
2. **Add physics** - Attach `OhaoPhysicsBody` to nodes
3. **Run** - Press F5, OHAO physics/rendering takes over
4. **Debug** - Use Godot's debugger + OHAO's console output

## TODO

- [ ] Implement OhaoViewport Vulkan rendering
- [ ] Implement scene sync (Godot -> OHAO)
- [ ] Implement physics body creation
- [ ] Add mesh rendering support
- [ ] Add material system integration
- [ ] Add gizmo overlays
