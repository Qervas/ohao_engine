#include "physics/module.hpp"

#include "physics/private/physics_scene_impl.hpp"

#include "modules/physics_dependencies.hpp"

namespace ohao::ecs {

namespace {

constexpr std::string_view kInputControl = "input_control";   // a game's control rule (PhysicsScene::setInputControl)

class PhysicsModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "physics"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kPhysicsDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerPhysicsTypes(types); }
    void install(World& world, const Services& services) const override { installPhysics(world, services.assets); }
    [[nodiscard]] std::span<const Method> methods() const override { return physicsMethods(); }
    // What a game adds to it: the control rule.
    [[nodiscard]] std::vector<std::string> contributions(const World& world) const override {
        const PhysicsScene* p = physicsScene(world);
        if (p && p->hasInputControl()) return {std::string(kInputControl)};
        return {};
    }
    void removeContributions(World& world, std::span<const std::string> names) const override {
        for (const std::string& n : names)
            if (n == kInputControl)
                if (PhysicsScene* p = physicsScene(world)) p->setInputControl({});
    }
};

} // namespace

const Module& physicsModule() {
    static const PhysicsModule m;
    return m;
}

} // namespace ohao::ecs
