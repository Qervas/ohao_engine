#include "world_render/private/scene_loads.hpp"

#include "render/scene/render_scene.hpp"

namespace ohao::ecs {

render::MeshHandle SceneLoads::uploadMesh(const cooked::Mesh& mesh, std::string_view debugName) {
    const render::MeshHandle h = run([&] { return render::uploadMesh(m_scene, mesh, debugName); });
    if (h.valid()) m_live.insert(key(h));
    return h;
}

render::MeshHandle SceneLoads::registerSkinnedInstance(render::MeshHandle base) {
    const render::MeshHandle h = run([&] {
        const render::MeshHandle own = m_scene.registerSkinnedInstance(base);
        return m_scene.meshInfo(own) ? own : render::MeshHandle{};
    });
    if (h.valid()) m_live.insert(key(h));
    return h;
}

void SceneLoads::releaseMesh(render::MeshHandle mesh) {
    m_live.erase(key(mesh));
    run([&] { m_scene.releaseMesh(mesh); });
}

render::MaterialHandle SceneLoads::registerMaterial(const render::MaterialDesc& desc) {
    return run([&] { return m_scene.registerMaterial(desc); });
}

void SceneLoads::releaseMaterial(render::MaterialHandle material) {
    run([&] { m_scene.releaseMaterial(material); });
}

render::MeshMaterials SceneLoads::meshMaterials(const cooked::Mesh& mesh, const render::TextureSource& open) {
    if (m_textures) m_unflushed = true;
    return run([&] { return render::meshMaterials(m_textures, mesh, open); });
}

BindlessTextureHandle SceneLoads::loadTexture(std::string_view path, BindlessTextureType type, bool generateMips) {
    if (!m_textures) return kInvalidBindlessTexture;
    m_unflushed = true;
    return run([&] { return m_textures->loadTexture(path, type, generateMips); });
}

void SceneLoads::releaseTexture(uint32_t texture) {
    if (m_textures) run([&] { m_textures->releaseContent(BindlessTextureHandle{texture}); });
}

void SceneLoads::flush() {
    if (!m_textures || !m_unflushed) return;
    m_unflushed = false;
    run([&] { m_textures->flushUploads(); });
}

} // namespace ohao::ecs
