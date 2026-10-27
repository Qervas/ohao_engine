#include "ecs/impact_marks.hpp"

#include "core/log.hpp"
#include "ecs/combat.hpp"
#include "ecs/components.hpp"
#include "ecs/world.hpp"
#include "render/scene/model_upload.hpp"
#include "render/scene/render_scene.hpp"
#include "scene/asset/model.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>

namespace ohao::ecs {

namespace {

struct ImpactMarksHolder {
    std::shared_ptr<ImpactMarks> marks;
};

// What `e` is drawn at: between steps (RenderPose) or at the step's pose.
glm::mat4 drawnWorld(const entt::registry& reg, entt::entity e) {
    if (const auto* rp = reg.try_get<RenderPose>(e)) return rp->world;
    if (const auto* wt = reg.try_get<WorldTransform>(e)) return wt->world;
    if (const auto* t = reg.try_get<Transform>(e)) return t->matrix();
    return glm::mat4(1.0f);
}

// Where `e` stands at this step: its Transforms up the chain (WorldTransform can be a step behind
// inside one, and RenderPose is a window's). A mark is measured against this, so every host, made
// early or late, attaches it alike.
glm::mat4 stepWorld(const entt::registry& reg, entt::entity e) {
    const auto* t = reg.try_get<Transform>(e);
    glm::mat4 m = t ? t->matrix() : glm::mat4(1.0f);
    int depth = 0;
    for (const auto* p = reg.try_get<Parent>(e); p && reg.valid(p->entity) && depth < 64; p = reg.try_get<Parent>(p->entity), ++depth)
        if (const auto* pt = reg.try_get<Transform>(p->entity)) m = pt->matrix() * m;
    return m;
}

// The quad's pose: 8 cm across, facing `normal` (its +Z), 2 mm off `point`.
glm::mat4 markPose(glm::vec3 point, glm::vec3 normal) {
    const glm::vec3 n = glm::length(normal) > 0.0f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 helper = std::fabs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 x = glm::normalize(glm::cross(helper, n));
    const glm::vec3 y = glm::cross(n, x);
    return glm::mat4(glm::vec4(x * ImpactMarks::kSize, 0.0f), glm::vec4(y * ImpactMarks::kSize, 0.0f), glm::vec4(n, 0.0f),
                     glm::vec4(point + n * ImpactMarks::kLift, 1.0f));
}

struct BoxHit {
    float t{std::numeric_limits<float>::max()};
    glm::vec3 point{0.0f}, normal{0.0f};
};

// The ray `origin + t dir` (world) against `e`'s BoxPrim and its descendants' (in their frames):
// the nearest entry with t >= 0.
void hitBoxes(const entt::registry& reg, entt::entity e, glm::vec3 origin, glm::vec3 dir, BoxHit& best, int depth) {
    if (depth > 4 || !reg.valid(e)) return;
    if (const auto* ms = reg.try_get<MeshSource>(e))
        if (const auto* box = std::get_if<BoxPrim>(&ms->source)) {
            const glm::mat4 world = stepWorld(reg, e);
            const glm::mat4 toLocal = glm::inverse(world);
            const glm::vec3 o(toLocal * glm::vec4(origin, 1.0f));
            const glm::vec3 d(toLocal * glm::vec4(dir, 0.0f));
            float t0 = -std::numeric_limits<float>::max(), t1 = std::numeric_limits<float>::max();
            int axis = -1;
            float sign = 0.0f;
            bool miss = false;
            for (int i = 0; i < 3 && !miss; ++i) {
                if (std::fabs(d[i]) < 1e-12f) {
                    miss = o[i] < box->lo[i] || o[i] > box->hi[i];
                    continue;
                }
                float a = (box->lo[i] - o[i]) / d[i], b = (box->hi[i] - o[i]) / d[i];
                float s = -1.0f;   // entering through the lo face: its normal is -axis
                if (a > b) {
                    std::swap(a, b);
                    s = 1.0f;
                }
                if (a > t0) {
                    t0 = a;
                    axis = i;
                    sign = s;
                }
                t1 = std::min(t1, b);
                miss = t0 > t1;
            }
            if (!miss && axis >= 0 && t0 >= 0.0f) {
                const glm::vec3 local = o + d * t0;
                const glm::vec3 p(world * glm::vec4(local, 1.0f));
                const float t = glm::length(p - origin);
                if (t < best.t) {
                    glm::vec3 n(0.0f);
                    n[axis] = sign;
                    best.t = t;
                    best.point = p;
                    best.normal = glm::normalize(glm::transpose(glm::mat3(toLocal)) * n);
                }
            }
        }
    if (const auto* ch = reg.try_get<Children>(e))
        for (const entt::entity c : ch->entities) hitBoxes(reg, c, origin, dir, best, depth + 1);
}

} // namespace

void ImpactMarks::add(const World& world, entt::entity on, glm::vec3 point, glm::vec3 normal) {
    const entt::registry& reg = world.registry();
    ImpactMark m;
    m.world = markPose(point, normal);
    if (on != entt::null && reg.valid(on)) {
        m.on = on;
        m.local = glm::inverse(stepWorld(reg, on)) * m.world;
    } else {
        m.local = m.world;
    }
    m.serial = ++m_serial;
    if (m_marks.size() < kCapacity) {
        m_marks.push_back(m);
    } else {
        m_marks[m_next] = m;
        m_next = (m_next + 1) % kCapacity;
    }
}

void ImpactMarks::clear() {
    m_marks.clear();
    m_next = 0;
}

void ImpactMarks::follow(const World& world) {
    const entt::registry& reg = world.registry();
    for (ImpactMark& m : m_marks)
        if (m.on != entt::null && reg.valid(m.on)) m.world = drawnWorld(reg, m.on) * m.local;
}

void ImpactMarks::observe(const World& world) {
    const CombatSystem* combat = combatSystem(world);
    if (!combat) return;
    const entt::registry& reg = world.registry();
    for (;;) {
        uint64_t dropped = 0;
        const auto events = combat->eventsSince(m_cursor, 256, dropped);
        if (events.empty()) break;
        for (const CombatEvent& e : events) {
            m_cursor = e.seq + 1;
            switch (e.kind) {
            case CombatEvent::Kind::Restart:
                clear();
                m_lastShot.clear();
                break;
            case CombatEvent::Kind::Shot:
                m_lastShot[e.sourceId] = e.normal;
                break;
            case CombatEvent::Kind::Hit: {
                const entt::entity target = e.target != entt::null && reg.valid(e.target) ? e.target : entt::entity{entt::null};
                const auto* pc = target != entt::null ? reg.try_get<PlayerController>(target) : nullptr;
                if (pc && pc->input) break;   // never on the player: in front of its own eyes
                if (pc) {
                    // A character: on its mesh boxes along the round's path, or not at all.
                    const auto shot = m_lastShot.find(e.sourceId);
                    const glm::vec3 dir = shot != m_lastShot.end() ? shot->second : -e.normal;
                    BoxHit best;
                    hitBoxes(reg, target, e.point - dir * 0.05f, dir, best, 0);
                    if (best.t < std::numeric_limits<float>::max()) add(world, target, best.point, best.normal);
                    break;
                }
                add(world, target, e.point, e.normal);
                break;
            }
            default:
                break;
            }
        }
    }
}

namespace {

// The marks as RenderScene instances of one dark quad, slot for slot.
class MarksRender {
public:
    MarksRender(render::RenderScene& scene, std::shared_ptr<ImpactMarks> marks)
        : m_scene(scene), m_marks(std::move(marks)) {}
    ~MarksRender() {
        for (const uint32_t slot : m_slots) m_scene.destroyInstance(slot);
        if (m_mesh.valid()) m_scene.releaseMesh(m_mesh);
    }

    void run(World& world) {
        m_marks->observe(world);
        m_marks->follow(world);
        const auto marks = m_marks->marks();
        if (marks.empty() && m_slots.empty()) return;
        if (!m_mesh.valid() && !makeMesh()) return;
        for (std::size_t i = 0; i < marks.size(); ++i) {
            render::InstanceDesc d;
            d.mesh = m_mesh;
            d.materials = std::span<const render::MaterialHandle>(&m_material, 1);
            d.world = marks[i].world;
            d.giAlbedoPacked = glm::vec4(0.05f, 0.05f, 0.05f, 0.95f);
            if (i >= m_slots.size()) {
                d.prevWorld = d.world;
                const uint32_t slot = m_scene.createInstance(d);
                if (slot == render::kInvalidIndex) return;
                m_slots.push_back(slot);
                m_drawn.push_back({marks[i].serial, d.world});
            } else if (m_drawn[i].serial != marks[i].serial || m_drawn[i].world != d.world) {
                // A new mark in a reused slot starts still; one moving with its entity keeps motion vectors.
                d.prevWorld = m_drawn[i].serial == marks[i].serial ? m_drawn[i].world : d.world;
                m_scene.updateInstance(m_slots[i], d);
                m_drawn[i] = {marks[i].serial, d.world};
            }
        }
        while (m_slots.size() > marks.size()) {   // cleared (a restart)
            m_scene.destroyInstance(m_slots.back());
            m_slots.pop_back();
            m_drawn.pop_back();
        }
    }

private:
    bool makeMesh() {
        Model model;
        const float h = 0.5f;
        PrimitiveMeshGenerator::generateQuad(model, {-h, -h, 0.0f}, {h, -h, 0.0f}, {h, h, 0.0f}, {-h, h, 0.0f},
                                             {0.0f, 0.0f, 1.0f}, glm::vec3(0.05f));
        m_mesh = render::uploadModelMesh(m_scene, model, "impact_mark");
        if (!m_mesh.valid()) {
            OHAO_LOG(Warn, ecs, "impact marks: the quad mesh could not be made; no marks are drawn");
            return false;
        }
        render::MaterialDesc mat;
        mat.baseColor = glm::vec3(0.035f, 0.032f, 0.03f);
        mat.roughness = 0.95f;
        m_material = m_scene.registerMaterial(mat);
        return true;
    }

    struct Drawn {
        uint64_t serial{0};
        glm::mat4 world{1.0f};
    };
    render::RenderScene& m_scene;
    std::shared_ptr<ImpactMarks> m_marks;
    render::MeshHandle m_mesh{};
    render::MaterialHandle m_material{};
    std::vector<uint32_t> m_slots;
    std::vector<Drawn> m_drawn;
};

std::shared_ptr<ImpactMarks> holderOf(World& world) {
    auto& ctx = world.registry().ctx();
    if (auto* h = ctx.find<ImpactMarksHolder>()) return h->marks;
    auto marks = std::make_shared<ImpactMarks>();
    ctx.emplace<ImpactMarksHolder>(ImpactMarksHolder{marks});
    return marks;
}

} // namespace

ImpactMarks& impactMarksOf(World& world) { return *holderOf(world); }

void installImpactMarks(World& world, render::RenderScene& scene) {
    auto state = std::make_shared<MarksRender>(scene, holderOf(world));
    const auto names = world.syncNames();
    const bool extract = std::find(names.begin(), names.end(), "render_extract") != names.end();
    if (extract) world.addSyncAfter("render_extract", "impact_marks", [state](World& w) { state->run(w); });
    else world.addSync("impact_marks", [state](World& w) { state->run(w); });
}

} // namespace ohao::ecs
