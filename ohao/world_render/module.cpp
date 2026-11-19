#include "world_render/module.hpp"

#include "world_render/present.hpp"
#include "world_render/private/scene_loads.hpp"
#include "world_render/private/systems.hpp"

#include "modules/world_render_dependencies.hpp"

#include <array>
#include <string>

namespace ohao::ecs {

namespace {

class WorldRenderModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "world_render"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kWorldRenderDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerWorldRenderTypes(types); }
    // The texture table is optional: a renderer without one (it failed to start) draws untextured.
    [[nodiscard]] std::span<const TypeKey> needs() const override {
        static constexpr std::array<TypeKey, 1> kNeeds{typeKey<render::RenderScene>()};
        return kNeeds;
    }
    void install(World& world, const Services& services) const override {
        // The host's render thread, when it has one (render/render_thread.hpp): the loads go through it.
        auto loads = std::make_shared<SceneLoads>(*services.get<render::RenderScene>(),
                                                  services.get<BindlessTextureManager>(),
                                                  services.get<render::RenderThread>());
        installInstantiate(world, loads, services.assets);
        installRenderExtract(world, loads, services.assets);
        installVfxDraw(world, loads, services.assets);
    }
    // What a game adds to it: its presenters (HUDs, spheres), by id.
    [[nodiscard]] std::vector<std::string> contributions(const World& world) const override {
        std::vector<std::string> out;
        for (const uint32_t id : presenterIds(world)) out.push_back(std::to_string(id));
        return out;
    }
    void removeContributions(World& world, std::span<const std::string> names) const override {
        for (const std::string& n : names) removePresenter(world, static_cast<uint32_t>(std::stoul(n)));
    }
};

} // namespace

const Module& worldRenderModule() {
    static const WorldRenderModule m;
    return m;
}

} // namespace ohao::ecs
