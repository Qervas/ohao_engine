#include "visual/module.hpp"

#include "modules/visual_dependencies.hpp"

namespace ohao::ecs {

namespace {

class VisualModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "visual"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kVisualDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerVisualTypes(types); }
    void install(World&, const Services&) const override {}
};

} // namespace

const Module& visualModule() {
    static const VisualModule m;
    return m;
}

} // namespace ohao::ecs
