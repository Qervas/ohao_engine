// The core module, "ecs" (ecs/module.hpp): the World's own types. Its systems (transform, spin, fly_controller) are
// installed by the World's constructor.
#include "ecs/module.hpp"

#include "modules/ecs_dependencies.hpp"

namespace ohao::ecs {

namespace {

class CoreModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "ecs"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kEcsDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerCoreTypes(types); }
    void install(World&, const Services&) const override {}
};

} // namespace

const Module& coreModule() {
    static const CoreModule m;
    return m;
}

} // namespace ohao::ecs
