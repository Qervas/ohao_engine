#include "footsteps/module.hpp"

#include "footsteps/footsteps.hpp"

#include "modules/footsteps_dependencies.hpp"

namespace ohao::ecs {

namespace {

class FootstepsModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "footsteps"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kFootstepsDependencies; }
    void install(World& world, const Services&) const override { installFootsteps(world); }
};

} // namespace

const Module& footstepsModule() {
    static const FootstepsModule m;
    return m;
}

} // namespace ohao::ecs
