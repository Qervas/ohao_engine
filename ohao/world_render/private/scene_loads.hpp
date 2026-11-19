#pragma once
// What the world's systems put into the renderer and give back (instantiate's meshes, materials and textures, the
// extract's skinned instances, vfx_draw's textures), and which meshes are live. A load and a release run through the
// host's render thread when it has one (render/render_thread.hpp: run waits for the frame being recorded, then runs
// here, on the world's thread), else at once; run(fn) holds the renderer for a batch of them. Which meshes are live
// is kept here, as this world registered and released them, so the extract asks it every frame without the
// renderer. One per world, shared by the module's systems (world_render/module.cpp).
#include "core/render_handles.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/render_thread.hpp"
#include "render/scene/model_upload.hpp"

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <string_view>
#include <utility>

namespace ohao::render {
class RenderScene;
}

namespace ohao::ecs {

class SceneLoads {
public:
    // `thread`: the host's render thread, or null (none: every call is direct).
    SceneLoads(render::RenderScene& scene, BindlessTextureManager* textures, render::RenderThread* thread)
        : m_scene(scene), m_textures(textures), m_thread(thread) {}

    // Runs `fn` holding the renderer (render/render_thread.hpp); the calls below inside it hold it already.
    template <class F>
    decltype(auto) run(F&& fn) {
        if (m_thread) return m_thread->run(std::forward<F>(fn));
        return std::forward<F>(fn)();
    }

    [[nodiscard]] BindlessTextureManager* textures() const noexcept { return m_textures; }

    // A cooked mesh uploaded (render/scene/model_upload.hpp); live once valid.
    render::MeshHandle uploadMesh(const cooked::Mesh& mesh, std::string_view debugName);
    // `base`'s skinned copy (RenderScene::registerSkinnedInstance); live once valid.
    render::MeshHandle registerSkinnedInstance(render::MeshHandle base);
    // Released at once (RenderScene::releaseMesh): no longer live.
    void releaseMesh(render::MeshHandle mesh);
    // No longer live, released by the frame packet (the extract's skinned meshes, after their instances move off).
    void forget(render::MeshHandle mesh) { m_live.erase(key(mesh)); }
    [[nodiscard]] bool live(render::MeshHandle mesh) const { return mesh.valid() && m_live.contains(key(mesh)); }

    render::MaterialHandle registerMaterial(const render::MaterialDesc& desc);
    void releaseMaterial(render::MaterialHandle material);
    // A mesh's own materials and their textures by content (render/scene/model_upload.hpp).
    render::MeshMaterials meshMaterials(const cooked::Mesh& mesh, const render::TextureSource& open);
    // A texture file into the table; its pixels reach the GPU at flush(). Invalid without a texture table.
    BindlessTextureHandle loadTexture(std::string_view path, BindlessTextureType type, bool generateMips);
    // A hold of a content texture given back (BindlessTextureManager::releaseContent).
    void releaseTexture(uint32_t texture);
    // The textures loaded since the last flush, uploaded (BindlessTextureManager::flushUploads): owed before a frame
    // samples them. Nothing when none were.
    void flush();

private:
    static uint64_t key(render::MeshHandle h) noexcept { return (uint64_t{h.index} << 32) | h.generation; }

    render::RenderScene& m_scene;
    BindlessTextureManager* m_textures;
    render::RenderThread* m_thread;
    ankerl::unordered_dense::set<uint64_t> m_live;
    bool m_unflushed{false};
};

} // namespace ohao::ecs
