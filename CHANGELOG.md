# Changelog

All notable changes to OHAO Engine will be documented in this file.

## [0.2.0] - 2025-01-16

### Major Architecture Change: Godot Integration

OHAO Engine has transitioned from a standalone GLFW/ImGui application to a **GDExtension plugin** for Godot 4.x. This enables integration with Godot's mature editor, scene system, and tooling.

### Added
- **GDExtension Integration**: OHAO now runs as a Godot 4.x plugin
- **OhaoViewport**: Custom viewport panel in Godot Editor displaying OHAO's Vulkan renderer
- **OffscreenRenderer**: Headless Vulkan rendering pipeline that outputs to a CPU-readable pixel buffer
- **macOS Support**: Full support for macOS via MoltenVK
- **Camera Controls**: Orbit, pan, and zoom controls in the OHAO viewport

### Deprecated
- **ImGui**: Removed Dear ImGui - Godot Editor now provides all UI
- **GLFW**: Removed GLFW window management - Godot handles windowing
- **Standalone Executable**: The `ohao_engine` standalone app has been removed
- **Legacy VulkanContext**: The old GLFW-based rendering context moved to legacy

### Removed
- `external/imgui/` - Dear ImGui library
- `src/renderer/legacy/` - GLFW-based VulkanContext and related code
- `src/engine/input/` - GLFW input handling (Godot handles input)
- `src/engine/serialization/` - Custom serialization (Godot handles scene files)
- `src/main.cpp` - Standalone application entry point
- `src/tests/` - Test directory (testing done via Godot integration)

### Changed
- **Build System**: Now uses SCons for GDExtension alongside CMake for engine libraries
- **Rendering**: OffscreenRenderer replaces VulkanContext for Godot integration
- **Scene Management**: Scene system retained but serialization handled by Godot

### Migration Notes

If upgrading from an earlier version:

1. The standalone executable no longer exists - use Godot Editor instead
2. Build the GDExtension with SCons: `cd godot_editor && scons platform=macos arch=arm64`
3. Open `godot_editor/project` in Godot 4.x to access the OHAO viewport
4. Custom scenes should be created using Godot's scene system

---

## [0.1.0] - 2024-12-xx

### Initial Release
- Vulkan-based rendering with PBR materials
- Custom physics simulation
- Actor-component system
- ImGui-based editor interface
- OBJ model loading
- Shadow mapping
- Multi-light support
