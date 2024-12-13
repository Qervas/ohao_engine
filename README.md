# OHAO Engine

OHAO Engine is my physics engine developing on Linux platforms(Fedora Linux), focusing on advanced rendering techniques, procedural generation, and physics simulation.


<div style="display: flex;">
  <div style="flex: 1;">
    <img src="image/README/1733708535551.png" alt="Image 1" style="width: 100%;">
  </div>
  <div style="flex: 1;">
    <img src="image/README/1734118037401.png" alt="Image 2" style="width: 100%;">
  </div>
</div>



## Building

### Required Dependencies

- CMake 3.20+
- Vulkan SDK
- GLFW3

### Installation on Fedora Linux

```bash
sudo dnf install cmake vulkan-devel glfw-devel git gcc-c++
```

### Clone the Repository

```bash
# Clone the repository with submodules
git clone --recursive https://github.com/Qervas/ohao-engine.git

# Or if you already cloned without --recursive:
git clone https://github.com/Qervas/ohao-engine.git
cd ohao-engine
git submodule update --init --recursive
```

### Build Steps

```bash
mkdir build
cd build
cmake ..
make -j$(nproc)  # Use multiple cores for faster building
```

### Running

```bash
./ohao_engine
```

### Controls

- **WASD**: Camera movement
- **Mouse**: Look around
- **Space/Ctrl**: Up/Down
- **Shift**: Speed up movement
- **Esc**: Exit

## Development Status

Currently in early development. Features being implemented:

- [X] Basic window creation
- [X] Vulkan initialization, validation layer, pipeline, rasterization
- [X] Load scene from obj file, including lighting and materials
- [X] Friendly camera control
- [X] User interface
- [ ] BRDF and illumination model switch

## Documentation

- [Technical Specification](docs/TECHNICAL_SPEC.md)
- More documentation will be added as the project develops

## Project Structure

```
ohao-engine/
├── src/             # Source files
├── shaders/         # GLSL shaders
├── external/        # External dependencies
│   └── imgui/      # Dear ImGui (docking branch)
├── docs/           # Documentation
└── assets/         # 3D models and textures
```

## License

[MIT License](LICENSE)

## Contributing

This project is currently in early development. Contribution guidelines will be added soon.

## Author

[Qervas](mailto:djmax96945147@outlook.com)

## Acknowledgments

- [Dear ImGui](https://github.com/ocornut/imgui) - Immediate mode GUI
- [Vulkan](https://www.vulkan.org/) - Graphics API
- [GLFW](https://www.glfw.org/) - Window creation and input
