// The core module, "ecs" (ecs/module.hpp): the World's own types and systems. A World installs no system itself
// (World::World); every host installs the core with the rest (engine::install), or alone ({"ecs"}).
#include "ecs/module.hpp"

#include "ecs/fly_controller_system.hpp"
#include "ecs/spin_system.hpp"
#include "ecs/transform_system.hpp"
#include "ecs/world.hpp"

#include "modules/ecs_dependencies.hpp"

#include <utility>

namespace ohao::ecs {

namespace {

class CoreModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "ecs"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kEcsDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerCoreTypes(types); }
    void install(World& world, const Services&) const override {
        // Engine-provided behaviour, once per fixed step: they move entities, before the physics and the gameplay
        // that read where entities are. The flying camera turns first, then what spins.
        world.addSimulate("spin", [](SystemContext& c, float dt) { updateSpin(c, dt); },
                          {.after = {"fly_controller"}, .before = {"physics"}, .reads = types<Spin>(),
                           .writes = types<Transform>()});
        world.addSimulate("fly_controller",
                          [](SystemContext& c, float dt) { updateFlyControllers(c, std::as_const(c.world()).input(), dt); },
                          {.before = {"physics"}, .reads = types<FlyController>(), .writes = types<Transform>()});
        // Structural: transforms are propagated (the other systems declare themselves around it; a module's move
        // rule adds its own access, ecs::tagMoves).
        world.addSync("transform", [](SystemContext& c) { updateTransforms(c.registry()); },
                      {.reads = types<Transform, Parent, Children>(),
                       .writes = types<WorldTransform, MovedLastFrame, RenderDirty, TransformDirty>(), .exclusive = true});
    }
};

} // namespace

const Module& coreModule() {
    static const CoreModule m;
    return m;
}

} // namespace ohao::ecs
