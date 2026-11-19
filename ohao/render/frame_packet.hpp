#pragma once
// The frame packet (step 4b): everything the renderer reads of a world, written by the world_render module's sync
// systems (world_render/frame_packet.hpp) and the host, and applied by VulkanRenderer::render at the frame's start,
// which empties it. The renderer never reads a World; this is the whole of what passes between them.
//
// Persistent state goes as changes: the instances by id (create, update, destroy), the meshes the instances stopped
// using, the light list when it changed. Per-frame state goes whole: the effects (the vfx frame, its flashes, the
// presented spheres), the skinning palettes, the view, the HUD and the frame's dt; a field left unset keeps the last
// frame's.
//
// A world may sync several times between two frames: the packet then holds every change since the last frame, and
// applying it leaves the scene as applying each sync's at once would, with the same table slots. An update merges
// into the instance's create or update already in the packet; a light list or an effects frame replaces the one
// before it when nothing between them takes or gives back a table slot (the scene's own light emitters and spheres
// take slots too), else it is applied in its turn. Ids are the writer's (world_render's render extract); the scene
// maps each to its table slot.
#include "render/hud/hud_list.hpp"
#include "render/scene/render_scene_types.hpp"
#include "render/scene/vfx_frame.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ohao::render {

class FramePacket {
public:
    // ---- Writing (the world's side) ----
    void createInstance(uint32_t id, const InstanceDesc& desc);
    void updateInstance(uint32_t id, const InstanceDesc& desc);
    void destroyInstance(uint32_t id);
    // Released after the instance changes before it (RenderScene::releaseMesh): an instance moves off a mesh first.
    void releaseMesh(MeshHandle mesh);
    // The scene's light list (RenderScene sorts it by order).
    void setLights(std::span<const LightDesc> lights);
    // What the frame draws of the world's effects (world_render/vfx_draw.hpp): every sync gives them whole.
    struct Effects {
        VfxFrame vfx;
        std::vector<LightDesc> flashes;          // lights of this frame only (LightKind::Flash)
        std::vector<PresentedSphere> spheres;    // a game's presented spheres
    };
    void setEffects(Effects effects);
    // A skinned mesh's palette this frame and last frame's (in its bone order), and a local box around its posed
    // vertices; the last given for a mesh wins.
    void setSkinPalette(MeshHandle skinned, std::span<const glm::mat4> current, std::span<const glm::mat4> previous,
                        const glm::vec3& boundsMin, const glm::vec3& boundsMax);

    // The world's camera and environment (visual/scene_view.hpp), resolved.
    struct Camera {
        glm::vec3 position{0.0f};
        float pitchDeg{0.0f};
        float yawDeg{-90.0f};
        float fovDeg{45.0f};
        float nearPlane{0.1f};
        float farPlane{100.0f};
    };
    struct Environment {
        std::string hdri;      // an absolute path, or empty: none
        bool sky{true};        // the physical sky lit by the scene's sun, without an hdri
        float intensity{1.0f};
    };
    struct View {
        std::optional<Camera> camera;   // none: the world has no active camera, and the renderer's stays
        Environment environment;
    };
    std::optional<View> view;
    // The HUD the deferred frames draw over their final image (empty: none).
    std::optional<HudList> hud;
    // The lines the deferred frames draw over their final image, before the HUD (empty: none).
    std::optional<std::vector<DebugLine>> lines;
    // The frame's wall time in seconds: auto exposure's adaptation.
    std::optional<float> frameDt;

    // ---- Reading (the renderer's side) ----
    enum class OpKind : uint8_t { Create, Update, Destroy, ReleaseMesh, Lights, Effects };
    struct Op {
        OpKind kind;
        uint32_t index;   // into instances(), releases(), lightSets() or effects(), by kind
    };
    struct Instance {
        uint32_t id{0};
        MeshHandle mesh{};
        uint32_t firstMaterial{0};
        uint32_t materialCount{0};
        glm::mat4 world{1.0f};
        glm::mat4 prevWorld{1.0f};
        bool visible{true};
        bool viewModel{false};
    };
    struct Skin {
        MeshHandle mesh{};
        uint32_t firstMatrix{0};   // current at [first, first + bones), previous at [first + bones, first + 2 bones)
        uint32_t bones{0};
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
    };
    [[nodiscard]] std::span<const Op> ops() const noexcept { return m_ops; }
    [[nodiscard]] const Instance& instance(uint32_t index) const { return m_instances[index]; }
    // The instance's description; its materials point into the packet.
    [[nodiscard]] InstanceDesc desc(const Instance& i) const;
    [[nodiscard]] MeshHandle release(uint32_t index) const { return m_releases[index]; }
    [[nodiscard]] std::span<const LightDesc> lightSet(uint32_t index) const { return m_lightSets[index]; }
    [[nodiscard]] const Effects& effects(uint32_t index) const { return m_effects[index]; }
    [[nodiscard]] std::span<const Skin> skins() const noexcept { return m_skins; }
    [[nodiscard]] std::span<const glm::mat4> current(const Skin& s) const {
        return {m_palettes.data() + s.firstMatrix, s.bones};
    }
    [[nodiscard]] std::span<const glm::mat4> previous(const Skin& s) const {
        return {m_palettes.data() + s.firstMatrix + s.bones, s.bones};
    }

    // Empties it, keeping its storage.
    void clear();
    // Hands what was written since the last frame to `to`, an empty packet (one the renderer applied), and takes
    // its storage: the render thread's double buffer (render/render_thread.hpp). What the writer keeps across frames
    // (the presented spheres' count) stays here.
    void moveTo(FramePacket& to);

private:
    static constexpr uint32_t kNone = UINT32_MAX;
    void writeInstance(Instance& to, const InstanceDesc& desc);
    // An op that takes or gives back a table slot: a light list or an effects frame before it is not replaced.
    void slotsMoved() noexcept { m_slotsMovedAt = static_cast<uint32_t>(m_ops.size()); }

    std::vector<Op> m_ops;
    std::vector<Instance> m_instances;
    std::vector<MaterialHandle> m_materials;
    std::vector<MeshHandle> m_releases;
    std::vector<std::vector<LightDesc>> m_lightSets;
    std::vector<Effects> m_effects;
    std::vector<Skin> m_skins;
    std::vector<glm::mat4> m_palettes;
    // By id: the index in m_instances of the instance's create or update in this packet (kNone: none, or destroyed
    // since). By mesh index: its skin. Reset entry by entry in clear().
    std::vector<uint32_t> m_latest;
    std::vector<uint32_t> m_skinOf;
    uint32_t m_lightsOp{kNone};    // the newest light list's op
    uint32_t m_effectsOp{kNone};   // the newest effects' op
    uint32_t m_slotsMovedAt{0};    // one past the newest op that took or gave back a slot (0: none)
    // The presented spheres the newest effects hold, kept across clear(): the count the scene holds once the packet
    // is applied. Effects with another count take or give back slots.
    std::size_t m_spheres{0};
};

} // namespace ohao::render
