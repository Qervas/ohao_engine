# ohao_vfx_effects (sub-project 12), below the vfx module in its directory: the effect set (.vfx.json) and the closed
# form every particle is drawn from. No ECS and no Vulkan: core, glm, and simdjson privately for JSON. The module's
# `vfx` system (vfx/vfx.cpp) writes spawn records from it; the renderer draws with it and evaluates the same closed
# form on the GPU.
add_library(ohao_vfx_effects STATIC vfx_set.cpp vfx_set.hpp particle_math.hpp smoke_math.hpp tracer_math.hpp)
target_include_directories(ohao_vfx_effects PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_vfx_effects PUBLIC cxx_std_20)
target_link_libraries(ohao_vfx_effects PUBLIC ohao_core glm PRIVATE ohao_simdjson)
