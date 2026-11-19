#include "render/frame_packet.hpp"

#include <algorithm>
#include <utility>

namespace ohao::render {

namespace {

// Grows `v` to hold index `i`, new entries `fill`.
void reach(std::vector<uint32_t>& v, uint32_t i, uint32_t fill) {
    if (i >= v.size()) v.resize(std::size_t{i} + 1, fill);
}

} // namespace

void FramePacket::writeInstance(Instance& to, const InstanceDesc& desc) {
    to.mesh = desc.mesh;
    to.world = desc.world;
    to.prevWorld = desc.prevWorld;
    to.visible = desc.visible;
    to.viewModel = desc.viewModel;
    const auto count = static_cast<uint32_t>(desc.materials.size());
    if (count > to.materialCount) to.firstMaterial = static_cast<uint32_t>(m_materials.size());   // a new range
    if (to.firstMaterial == m_materials.size()) m_materials.resize(m_materials.size() + count);
    std::copy(desc.materials.begin(), desc.materials.end(), m_materials.begin() + to.firstMaterial);
    to.materialCount = count;
}

void FramePacket::createInstance(uint32_t id, const InstanceDesc& desc) {
    reach(m_latest, id, kNone);
    const auto index = static_cast<uint32_t>(m_instances.size());
    Instance& i = m_instances.emplace_back();
    i.id = id;
    i.firstMaterial = static_cast<uint32_t>(m_materials.size());
    writeInstance(i, desc);
    m_ops.push_back({OpKind::Create, index});
    m_latest[id] = index;
    slotsMoved();
}

void FramePacket::updateInstance(uint32_t id, const InstanceDesc& desc) {
    reach(m_latest, id, kNone);
    if (const uint32_t at = m_latest[id]; at != kNone) {   // merged into its create or update in this packet
        writeInstance(m_instances[at], desc);
        return;
    }
    const auto index = static_cast<uint32_t>(m_instances.size());
    Instance& i = m_instances.emplace_back();
    i.id = id;
    i.firstMaterial = static_cast<uint32_t>(m_materials.size());
    writeInstance(i, desc);
    m_ops.push_back({OpKind::Update, index});
    m_latest[id] = index;
}

void FramePacket::destroyInstance(uint32_t id) {
    reach(m_latest, id, kNone);
    const auto index = static_cast<uint32_t>(m_instances.size());
    m_instances.push_back(Instance{.id = id});
    m_ops.push_back({OpKind::Destroy, index});
    m_latest[id] = kNone;
    slotsMoved();
}

void FramePacket::releaseMesh(MeshHandle mesh) {
    m_ops.push_back({OpKind::ReleaseMesh, static_cast<uint32_t>(m_releases.size())});
    m_releases.push_back(mesh);
}

void FramePacket::setLights(std::span<const LightDesc> lights) {
    // Replaces the newest list when no slot moved since it: the scene ends with the same slots.
    if (m_lightsOp != kNone && m_slotsMovedAt <= m_lightsOp) {
        m_lightSets[m_ops[m_lightsOp].index].assign(lights.begin(), lights.end());
        return;
    }
    m_lightsOp = static_cast<uint32_t>(m_ops.size());
    m_ops.push_back({OpKind::Lights, static_cast<uint32_t>(m_lightSets.size())});
    m_lightSets.emplace_back(lights.begin(), lights.end());
}

void FramePacket::setEffects(Effects effects) {
    const std::size_t spheres = effects.spheres.size();
    // As many spheres as the newest effects: in its place, they rewrite the same slots.
    if (m_effectsOp != kNone && spheres == m_spheres) {
        m_effects[m_ops[m_effectsOp].index] = std::move(effects);
        return;
    }
    m_effectsOp = static_cast<uint32_t>(m_ops.size());
    m_ops.push_back({OpKind::Effects, static_cast<uint32_t>(m_effects.size())});
    m_effects.push_back(std::move(effects));
    if (spheres != m_spheres) slotsMoved();
    m_spheres = spheres;
}

void FramePacket::setSkinPalette(MeshHandle skinned, std::span<const glm::mat4> current,
                                 std::span<const glm::mat4> previous, const glm::vec3& boundsMin,
                                 const glm::vec3& boundsMax) {
    if (current.size() != previous.size()) return;
    const auto bones = static_cast<uint32_t>(current.size());
    reach(m_skinOf, skinned.index, kNone);
    uint32_t& at = m_skinOf[skinned.index];
    if (at == kNone || m_skins[at].mesh.generation != skinned.generation) {
        at = static_cast<uint32_t>(m_skins.size());
        m_skins.push_back(Skin{.mesh = skinned});
    }
    Skin& s = m_skins[at];
    if (bones != s.bones || s.firstMatrix + 2 * bones > m_palettes.size()) {
        s.firstMatrix = static_cast<uint32_t>(m_palettes.size());
        m_palettes.resize(m_palettes.size() + 2 * std::size_t{bones});
    }
    s.bones = bones;
    std::copy(current.begin(), current.end(), m_palettes.begin() + s.firstMatrix);
    std::copy(previous.begin(), previous.end(), m_palettes.begin() + s.firstMatrix + bones);
    s.boundsMin = boundsMin;
    s.boundsMax = boundsMax;
}

InstanceDesc FramePacket::desc(const Instance& i) const {
    InstanceDesc d{};
    d.mesh = i.mesh;
    d.materials = std::span<const MaterialHandle>(m_materials.data() + i.firstMaterial, i.materialCount);
    d.world = i.world;
    d.prevWorld = i.prevWorld;
    d.visible = i.visible;
    d.viewModel = i.viewModel;
    return d;
}

void FramePacket::clear() {
    for (const Instance& i : m_instances) m_latest[i.id] = kNone;
    for (const Skin& s : m_skins) m_skinOf[s.mesh.index] = kNone;
    m_ops.clear();
    m_instances.clear();
    m_materials.clear();
    m_releases.clear();
    m_lightSets.clear();
    m_effects.clear();
    m_skins.clear();
    m_palettes.clear();
    m_lightsOp = m_effectsOp = kNone;
    m_slotsMovedAt = 0;
    view.reset();
    hud.reset();
    frameDt.reset();
}

void FramePacket::moveTo(FramePacket& to) {
    const std::size_t spheres = m_spheres;
    std::swap(*this, to);
    m_spheres = spheres;
}

} // namespace ohao::render
