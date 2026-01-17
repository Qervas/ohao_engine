# OHAO Engine - Claude Context

## Project Overview

OHAO Engine is a custom Vulkan renderer and physics engine integrated into Godot 4.3+ as a GDExtension. The goal is to use Godot as the editor UI while rendering with OHAO's custom Vulkan pipeline.

## Build Commands

```bash
# Build OHAO core libraries (from ohao_engine/build/)
cd /Users/franktudor/Documents/Github/ohao_engine/build
cmake --build . -j8

# Build GDExtension (from godot_editor/)
cd /Users/franktudor/Documents/Github/ohao_engine/godot_editor
scons arch=arm64 -j8

# Run Godot editor
cd project && godot -e
```

## Key Files

### GDExtension (godot_editor/src/)
- `ohao_viewport.cpp/.h` - Main viewport that renders using OHAO's Vulkan renderer
- `ohao_physics_body.cpp/.h` - Physics body wrapper
- `register_types.cpp` - GDExtension registration

### OHAO Renderer (src/renderer/offscreen/)
- `offscreen_renderer.cpp/.hpp` - Main offscreen Vulkan renderer
- `offscreen_buffers.cpp` - Vertex/index/uniform buffer management
- `offscreen_pipeline.cpp` - Graphics pipeline creation
- `offscreen_scene_render.cpp` - Scene mesh rendering

### Godot Plugin (project/addons/ohao/)
- `ohao_editor_main.gd` - Main editor script
- `ohao_editor_main.tscn` - Editor UI layout

### Shaders (shaders/)
- `offscreen_simple.vert/frag` - Shaders for offscreen rendering with lighting

## Architecture

```
Godot Editor
    │
    ▼
OhaoViewport (GDExtension Control node)
    │ calls
    ▼
OffscreenRenderer (C++ Vulkan renderer)
    │
    ├─ Renders to offscreen framebuffer
    ├─ Copies pixels to CPU buffer
    │
    ▼
OhaoViewport copies pixels to Godot ImageTexture
    │
    ▼
Displayed in Godot Editor UI
```

## Completed Work

1. **Phase 1-2**: Basic Vulkan pipeline, vertex/index buffers, scene mesh rendering
2. **Phase 3**: Dynamic lighting (directional, point, spot lights - up to 8)
3. **Phase 5**: Scene sync from Godot (MeshInstance3D, lights, materials)
4. **Workflow**: Example scenes, "OHAO Engine" text on empty viewport, hot reload

## Current State

- OHAO viewport tab works in Godot editor
- Can sync scenes from Godot's 3D editor to OHAO renderer
- Supports basic meshes (Box, Sphere, Cylinder, Plane)
- Dynamic lighting with up to 8 lights
- Camera controls (WASD + mouse)
- Empty viewport shows "OHAO Engine" text

## Important Notes

- Build arm64 only (no intel mac support needed): `scons arch=arm64 -j8`
- Libraries are in `ohao_engine/build/` (cmake output)
- GDExtension links against those libraries via SConstruct
- Hot reload works but sometimes requires Godot restart

## Next Steps (TODO)

1. Shadow rendering
2. Custom mesh import (OBJ, GLTF)
3. Physics simulation integration
4. Wireframe mode toggle
5. Grid overlay
6. Gizmo overlays
7. Real-time scene sync (auto-update on changes)
