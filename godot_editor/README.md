# OHAO Engine - Godot Editor Integration

Use Godot 4.3+ as the editor UI while using OHAO's Vulkan renderer and physics engine.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Godot Editor                          │
│  ┌──────────┐  ┌─────────────────────┐  ┌────────────┐  │
│  │Scene Tree│  │   OHAO Viewport     │  │ Inspector  │  │
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
3. **Vulkan SDK** - MoltenVK on macOS via Homebrew: `brew install molten-vk`

## Building

### 1. Build OHAO Core Libraries (cmake)

```bash
cd /path/to/ohao_engine/build
cmake ..
cmake --build . -j8
```

### 2. Build GDExtension (scons)

```bash
cd godot_editor

# Build for Apple Silicon (arm64) - recommended
scons arch=arm64 -j8

# Or build universal (slower, larger)
# scons -j8
```

### 3. Run Godot

```bash
cd project
godot -e
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
│   │   ├── ohao.gdextension
│   │   └── libohao.*.dylib
│   ├── addons/ohao/    # Editor plugin
│   │   ├── plugin.cfg
│   │   ├── ohao_editor_main.gd
│   │   └── ohao_editor_main.tscn
│   └── examples/       # Example scenes
│       ├── basic_scene.tscn
│       └── multi_light_scene.tscn
└── SConstruct          # Build script
```

## Workflow

1. **Open Godot** - The OHAO tab appears in the editor
2. **Create/Open a scene** - Use Godot's 3D editor to build scenes
3. **Sync to OHAO** - Click "Sync from Editor" to render with OHAO's Vulkan renderer
4. **Use Examples** - Select from the Examples dropdown to load pre-made scenes

### Supported Node Types

When syncing from Godot to OHAO:
- `MeshInstance3D` with `BoxMesh`, `SphereMesh`, `CylinderMesh`, `PlaneMesh`
- `DirectionalLight3D`
- `OmniLight3D` (point lights)
- `SpotLight3D`
- `Camera3D`

## Camera Controls (in OHAO Viewport)

- **Right-click + drag** - Look around
- **WASD** - Move camera
- **Shift** - Move faster
- **Arrow keys** - Rotate camera

## Custom Nodes

### OhaoViewport
Renders using OHAO's Vulkan pipeline.

```gdscript
var viewport = OhaoViewport.new()
viewport.sync_from_godot(scene_root)  # Sync Godot scene to OHAO
```

### OhaoPhysicsBody
Physics body using OHAO's custom physics engine.

```gdscript
var body = OhaoPhysicsBody.new()
body.body_type = OhaoPhysicsBody.BODY_DYNAMIC
body.mass = 1.0
body.shape_type = OhaoPhysicsBody.SHAPE_BOX
```

## Hot Reload

The GDExtension supports hot reload. After rebuilding:
1. Rebuild: `scons arch=arm64 -j8`
2. In Godot: The extension should reload automatically
3. If not, restart Godot

## Completed Features

- [x] Vulkan offscreen rendering pipeline
- [x] Scene sync (Godot -> OHAO)
- [x] Mesh rendering (Box, Sphere, Cylinder, Plane)
- [x] Dynamic lighting (Directional, Point, Spot - up to 8 lights)
- [x] Camera controls (FPS-style)
- [x] Material colors from StandardMaterial3D
- [x] Example scenes
- [x] Hot reload support

## TODO

- [ ] Shadow rendering
- [ ] Custom mesh import (OBJ, GLTF)
- [ ] Physics simulation integration
- [ ] Wireframe mode toggle
- [ ] Grid overlay
- [ ] Gizmo overlays
- [ ] Real-time scene sync (auto-update on changes)
