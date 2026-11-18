#include "vfx/module.hpp"

#include "vfx/vfx.hpp"

#include "modules/vfx_dependencies.hpp"

namespace ohao::ecs {

// Installs the `vfx` simulate system (after animation and the writers of what it binds) and its snapshot hook once
// per World (a second call returns the first; vfx/vfx.cpp).
VfxSystem& installVfx(World& world, AssetResolver& assets);

namespace {

class VfxModule final : public Module {
public:
    [[nodiscard]] std::string_view name() const override { return "vfx"; }
    [[nodiscard]] std::span<const std::string_view> dependencies() const override { return kVfxDependencies; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerVfxTypes(types); }
    void install(World& world, const Services& services) const override { installVfx(world, services.assets); }
};

} // namespace

const Module& vfxModule() {
    static const VfxModule m;
    return m;
}

} // namespace ohao::ecs
