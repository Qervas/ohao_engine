// RenderScene's instance and mesh tables as the raster passes read them (std430; matches
// ohao/render/scene/render_scene_types.hpp). Matrices are column-major (slangc
// -matrix-layout-column-major), as glm writes them.
module scene_data;

public struct InstanceGpu
{
    public float4x4 world;
    public float4x4 prevWorld;
    public uint mesh;
    public uint materialRefOffset;
    public uint materialRefCount;
    public uint flags;
    public float4 emitterLight;   // an emitter mesh's radiance (rgb) and light index (w)
};

public struct MeshGpu
{
    public uint vertexOffset;
    public uint indexOffset;
    public uint indexCount;
    public uint triangleSlotOffset;
};

public static const uint NO_TEXTURE = 0xFFFFFFFFu;
