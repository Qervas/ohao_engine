#include "world_render/present.hpp"

#include "ecs/world.hpp"

namespace ohao::ecs {

namespace {

struct Presenters {
    std::vector<HudFn> huds;
    std::vector<SpheresFn> spheres;
};

Presenters& presentersOf(World& world) {
    auto& ctx = world.registry().ctx();
    if (auto* p = ctx.find<Presenters>()) return *p;
    return ctx.emplace<Presenters>();
}

} // namespace

void addHud(World& world, HudFn fn) { presentersOf(world).huds.push_back(std::move(fn)); }

void addSpheres(World& world, SpheresFn fn) { presentersOf(world).spheres.push_back(std::move(fn)); }

render::HudList drawHuds(World& world, uint32_t width, uint32_t height) {
    render::HudList out;
    if (const auto* p = world.registry().ctx().find<Presenters>())
        for (const HudFn& fn : p->huds) fn(world, out, width, height);
    return out;
}

std::vector<render::RenderScene::PresentedSphere> presentedSpheres(const World& world) {
    std::vector<render::RenderScene::PresentedSphere> out;
    if (const auto* p = world.registry().ctx().find<Presenters>())
        for (const SpheresFn& fn : p->spheres) fn(world, out);
    return out;
}

} // namespace ohao::ecs
