#include "engine/install.hpp"

#include "ecs/animation.hpp"
#include "ecs/audio_schedule.hpp"
#include "ecs/module.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/vfx.hpp"
#include "level/level_system.hpp"
#include "world_render/instantiate.hpp"
#include "world_render/render_extract.hpp"
#include "world_render/vfx_draw.hpp"

#include <array>
#include <vector>

namespace ohao::engine {

namespace {

// A module whose code is not yet in its own directory (each becomes its module's module.cpp): its name, its
// dependencies, its registrations and its install.
class StagedModule final : public ecs::Module {
public:
    using Register = void (*)(reflect::TypeRegistry&);
    using Install = void (*)(ecs::World&, const ecs::Services&);
    StagedModule(std::string_view name, std::span<const std::string_view> deps, Register reg, Install install)
        : m_name(name), m_deps(deps), m_register(reg), m_install(install) {}
    [[nodiscard]] std::string_view name() const override { return m_name; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return m_deps; }
    void registerTypes(reflect::TypeRegistry& types) const override { m_register(types); }
    void install(ecs::World& world, const ecs::Services& services) const override {
        if (m_install) m_install(world, services);
    }

private:
    std::string_view m_name;
    std::span<const std::string_view> m_deps;
    Register m_register;
    Install m_install;
};

constexpr std::array<std::string_view, 1> kVisualDeps{"ecs"};
constexpr std::array<std::string_view, 2> kAnimationDeps{"ecs", "visual"};
constexpr std::array<std::string_view, 2> kPhysicsDeps{"ecs", "visual"};
constexpr std::array<std::string_view, 4> kAudioDeps{"ecs", "visual", "physics", "animation"};
constexpr std::array<std::string_view, 3> kVfxDeps{"ecs", "visual", "physics"};
constexpr std::array<std::string_view, 5> kLevelDeps{"ecs", "visual", "physics", "audio", "vfx"};
constexpr std::array<std::string_view, 4> kWorldRenderDeps{"ecs", "visual", "animation", "vfx"};

const StagedModule kVisual{"visual", kVisualDeps, &ecs::registerVisualTypes, nullptr};
const StagedModule kAnimation{"animation", kAnimationDeps, &ecs::registerAnimationTypes,
                              [](ecs::World& w, const ecs::Services& s) { ecs::installAnimation(w, s.assets); }};
const StagedModule kPhysics{"physics", kPhysicsDeps, &ecs::registerPhysicsTypes,
                            [](ecs::World& w, const ecs::Services& s) { ecs::installPhysics(w, s.assets); }};
const StagedModule kAudio{"audio", kAudioDeps, &ecs::registerAudioTypes,
                          [](ecs::World& w, const ecs::Services& s) { ecs::installAudio(w, s.assets); }};
const StagedModule kVfx{"vfx", kVfxDeps, &ecs::registerVfxTypes,
                        [](ecs::World& w, const ecs::Services& s) { ecs::installVfx(w, s.assets); }};
const StagedModule kLevel{"level", kLevelDeps, &ecs::registerLevelTypes,
                          [](ecs::World& w, const ecs::Services& s) { level::installLevels(w, s.assets, s.types); }};
// Its systems are installRendering's.
const StagedModule kWorldRender{"world_render", kWorldRenderDeps, &ecs::registerWorldRenderTypes, nullptr};

} // namespace

std::span<const ecs::Module* const> modules() {
    static const std::array<const ecs::Module*, 8> all{&ecs::coreModule(), &kVisual, &kAnimation, &kPhysics,
                                                       &kAudio,            &kVfx,    &kLevel,     &kWorldRender};
    return all;
}

void registerTypes(reflect::TypeRegistry& types) { ecs::registerModuleTypes(types, modules()); }

void install(ecs::World& world, const ecs::Services& services, std::span<const std::string_view> only) {
    if (only.empty()) return ecs::installModules(world, modules(), services);
    const std::vector<const ecs::Module*> chosen = ecs::withDependencies(modules(), only);
    ecs::installModules(world, chosen, services);
}

void installRendering(ecs::World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                      ecs::AssetResolver& assets) {
    ecs::installInstantiate(world, scene, textures, assets);
    ecs::installRenderExtract(world, scene);
    ecs::installVfxDraw(world, scene, textures, assets);
}

} // namespace ohao::engine
