#include "level/module.hpp"

#include "level/level_system.hpp"

#include "modules/level_dependencies.hpp"

namespace ohao::level {
// Adds the "level" sync system (before "transform") and its state once per World; the resolver
// and registry must outlive it (level/level_system.cpp).
void installLevels(ecs::World& world, ecs::AssetResolver& assets, const reflect::TypeRegistry& types);
} // namespace ohao::level

namespace ohao::ecs {

namespace {

class LevelModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "level"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kLevelDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerLevelTypes(types); }
    void install(World& world, const Services& services) const override {
        level::installLevels(world, services.assets, services.types);
    }
    [[nodiscard]] std::span<const Method> methods() const override { return levelMethods(); }
};

} // namespace

const Module& levelModule() {
    static const LevelModule m;
    return m;
}

} // namespace ohao::ecs
