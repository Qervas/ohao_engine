#include "animation/module.hpp"

#include "animation/animation.hpp"

#include "modules/animation_dependencies.hpp"

namespace ohao::ecs {

// Installs the systems once per World (a second call returns the first); the AssetResolver must outlive the World
// (animation/animation.cpp).
AnimationSystem& installAnimation(World& world, AssetResolver& assets);

namespace {

class AnimationModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "animation"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kAnimationDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerAnimationTypes(types); }
    void install(World& world, const Services& services) const override { installAnimation(world, services.assets); }
    [[nodiscard]] std::span<const Method> methods() const override { return animationMethods(); }
    // What a game adds to it: its drivers, by name.
    [[nodiscard]] std::vector<std::string> contributions(const World& world) const override {
        const AnimationSystem* a = animationSystem(world);
        return a ? a->driverNames() : std::vector<std::string>{};
    }
    void removeContributions(World& world, std::span<const std::string> names) const override {
        if (AnimationSystem* a = animationSystem(world))
            for (const std::string& n : names) a->removeDriver(n);
    }
};

} // namespace

const Module& animationModule() {
    static const AnimationModule m;
    return m;
}

} // namespace ohao::ecs
