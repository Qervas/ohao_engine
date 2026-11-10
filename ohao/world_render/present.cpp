#include "world_render/present.hpp"

#include "ecs/world.hpp"

namespace ohao::ecs {

namespace {

struct Presenters {
    std::vector<std::pair<uint32_t, HudFn>> huds;
    std::vector<std::pair<uint32_t, SpheresFn>> spheres;
    uint32_t next{1};
};

Presenters& presentersOf(World& world) {
    auto& ctx = world.registry().ctx();
    if (auto* p = ctx.find<Presenters>()) return *p;
    return ctx.emplace<Presenters>();
}

} // namespace

uint32_t addHud(World& world, HudFn fn) {
    Presenters& p = presentersOf(world);
    p.huds.emplace_back(p.next, std::move(fn));
    return p.next++;
}

uint32_t addSpheres(World& world, SpheresFn fn) {
    Presenters& p = presentersOf(world);
    p.spheres.emplace_back(p.next, std::move(fn));
    return p.next++;
}

void removePresenter(World& world, uint32_t id) {
    Presenters& p = presentersOf(world);
    std::erase_if(p.huds, [&](const auto& h) { return h.first == id; });
    std::erase_if(p.spheres, [&](const auto& s) { return s.first == id; });
}

std::vector<uint32_t> presenterIds(const World& world) {
    std::vector<uint32_t> out;
    if (const auto* p = world.registry().ctx().find<Presenters>()) {
        for (const auto& h : p->huds) out.push_back(h.first);
        for (const auto& s : p->spheres) out.push_back(s.first);
    }
    return out;
}

render::HudList drawHuds(World& world, uint32_t width, uint32_t height) {
    render::HudList out;
    if (const auto* p = world.registry().ctx().find<Presenters>())
        for (const auto& [id, fn] : p->huds) fn(world, out, width, height);
    return out;
}

std::vector<render::RenderScene::PresentedSphere> presentedSpheres(const World& world) {
    std::vector<render::RenderScene::PresentedSphere> out;
    if (const auto* p = world.registry().ctx().find<Presenters>())
        for (const auto& [id, fn] : p->spheres) fn(world, out);
    return out;
}

} // namespace ohao::ecs
