#include "navigation/module.hpp"

#include "ecs/world.hpp"

#include "modules/navigation_dependencies.hpp"

namespace ohao::ecs {

// navigation/navigation.cpp: the `navigation` system and its state, once per World.
void installNavigation(World& world, AssetResolver& assets);

namespace {

class NavigationModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "navigation"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kNavigationDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerNavigationTypes(types); }
    void install(World& world, const Services& services) const override { installNavigation(world, services.assets); }
    [[nodiscard]] std::span<const Method> methods() const override { return navigationMethods(); }
};

} // namespace

const Module& navigationModule() {
    static const NavigationModule m;
    return m;
}

} // namespace ohao::ecs
