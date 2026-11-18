# The world_render module (world_render/module.hpp): the world into the renderer (system design 4.1, step 3a):
# instantiate (MeshSources into meshes), the render extract, the effects' draw and what a game presents. The world's
# other modules do not link the renderer; this one does.
ohao_add_module(world_render DEPENDS ecs visual animation vfx LINKS ohao_renderer ohao_gpu_vulkan)
