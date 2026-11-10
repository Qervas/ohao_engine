#pragma once
// What a game presents beside the world's entities (step 3i): its HUDs and its presented spheres, each a function it
// registers on the World. The hosts draw the HUDs (the window, a capture with hud: true); vfx_draw gives the renderer
// the spheres each sync. Presentation only: never saved or hashed, and no step reads it.
#include "render/hud/hud_list.hpp"
#include "render/scene/render_scene.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace ohao::ecs {

class World;

// Appends a HUD's items for a width x height frame. It may read the world's events (with a cursor of its own).
using HudFn = std::function<void(World&, render::HudList&, uint32_t width, uint32_t height)>;
// Appends the spheres the frame draws, placed at the World's render alpha (the slice's grenades in flight).
using SpheresFn = std::function<void(const World&, std::vector<render::RenderScene::PresentedSphere>&)>;

// Each returns the presenter's id (removePresenter).
uint32_t addHud(World& world, HudFn fn);
uint32_t addSpheres(World& world, SpheresFn fn);
void removePresenter(World& world, uint32_t id);
[[nodiscard]] std::vector<uint32_t> presenterIds(const World& world);

// Every registered HUD's items, in registration order.
[[nodiscard]] render::HudList drawHuds(World& world, uint32_t width, uint32_t height);
// Every registered source's spheres, in registration order.
[[nodiscard]] std::vector<render::RenderScene::PresentedSphere> presentedSpheres(const World& world);

} // namespace ohao::ecs
