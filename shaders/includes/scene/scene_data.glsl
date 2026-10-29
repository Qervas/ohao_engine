#ifndef OHAO_SCENE_DATA_GLSL
#define OHAO_SCENE_DATA_GLSL
// Matches ohao/render/scene/render_scene_types.hpp (std430).
struct InstanceGpu {
    mat4 world;
    mat4 prevWorld;
    uint mesh;
    uint materialRefOffset;
    uint materialRefCount;
    uint flags;
    vec4 emitterLight;   // an emitter mesh's radiance (rgb) and light index (w)
};
struct MeshGpu { uint vertexOffset; uint indexOffset; uint indexCount; uint triangleSlotOffset; };
const uint NO_TEXTURE = 0xFFFFFFFFu;

// Material index for one submesh/triangle slot; missing refs use material 0 (default).
#define OHAO_MATERIAL_INDEX(inst, slot, refs) \
    ((slot) < (inst).materialRefCount ? (refs)[(inst).materialRefOffset + (slot)] : 0u)
#endif
