# OHAO Engine - Godot Editor Integration

See root `../CLAUDE.md` for full API reference, constants, and quick start.

## Build (Windows)

```bash
# Core C++ libs (from repo root)
cmake --build build --config Release -j8

# GDExtension DLL (from godot_editor/)
scons platform=windows -j8

# Run Godot editor
cd project && godot -e
```

## Key Files

### GDExtension C++ (src/)
- `ohao_viewport.h/cpp` - Main viewport orchestrator (delegates to sub-objects)
- `camera_controller.h/cpp` - FPS/Orbit camera + input handling
- `render_settings.h/cpp` - Post-processing configuration
- `scene_sync.h/cpp` - Godot <-> OHAO scene bridge
- `selection_controller.h/cpp` - Object picking and selection
- `ohao_physics_body.h/cpp` - Physics body wrapper
- `register_types.cpp` - GDExtension registration

### GDScript Helper Layer (project/addons/ohao_helpers/)
- `ohao_helpers.gd` - `Ohao` autoload singleton (viewport lookup, factories)
- `ohao_constants.gd` - `OhaoConst` named constants (no magic numbers)
- `ohao_scene_builder.gd` - `OhaoSceneBuilder` declarative scene building
- `ohao_presets.gd` - `OhaoPresets` rendering/scene presets
- `ohao_physics_helpers.gd` - `OhaoPhysicsHelpers` physics body factories

### Godot Plugin (project/addons/ohao/)
- `ohao_editor_main.gd` - Editor plugin with UI panels
- `ohao_editor_main.tscn` - Editor UI layout

## Architecture

```
Godot Editor
    |
    v
OhaoViewport (GDExtension Control)
    |-- CameraController     (FPS/Orbit camera)
    |-- RenderSettings       (Post-processing state)
    |-- SceneSync            (Godot <-> OHAO bridge)
    |-- SelectionController  (Picking/selection)
    |
    v
OffscreenRenderer (C++ Vulkan) -> ImageTexture -> Godot display
```

## Notes

- SConstruct uses `Glob("src/*.cpp")` - new .cpp files are auto-discovered
- After modifying `.h` files externally, delete `.sconsign.dblite` before rebuild
- Libraries link in dependency order (dependents before providers)
- `Ohao` autoload registered in `project.godot` - provides `Ohao.viewport()`, `Ohao.scene()`, etc.
