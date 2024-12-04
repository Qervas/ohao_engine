# OHAO Engine

OHAO Engine is my physics engine developing on Linux platforms(Fedora Linux), focusing on advanced rendering techniques, procedural generation, and physics simulation.


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
- [x] Basic window creation
- [x] Vulkan initialization, validation layer, pipeline, rasterization
- [x] Load scene from obj file, including lighting and materials
- [x] Friendly camera control
- [ ] User interface

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
