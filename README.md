# OHAO Engine

OHAO Engine is my physics engine developing on Linux platforms(Fedora Linux), focusing on advanced rendering techniques, procedural generation, and physics simulation.


## Building

### Prerequisites
- CMake 3.20+
- Vulkan SDK
- GLFW3

### Build Steps
```bash
mkdir build
cd build
cmake ..
make
```

### Running
```bash
./ohao_engine
```

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

## License

[MIT License](LICENSE)

## Contributing

This project is currently in early development. Contribution guidelines will be added soon.

## Authors

[Qervas](mailto:djmax96945147@outlook.com)
