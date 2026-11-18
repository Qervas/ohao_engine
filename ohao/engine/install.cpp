#include "engine/install.hpp"

#include "animation/module.hpp"
#include "audio/module.hpp"
#include "ecs/module.hpp"
#include "footsteps/module.hpp"
#include "level/module.hpp"
#include "physics/module.hpp"
#include "vfx/module.hpp"
#include "visual/module.hpp"
#include "world_render/module.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace ohao::engine {

std::span<const ecs::Module* const> modules() {
    static const std::array<const ecs::Module*, 9> all{&ecs::coreModule(),    &ecs::visualModule(), &ecs::animationModule(),
                                                       &ecs::physicsModule(), &ecs::audioModule(),  &ecs::footstepsModule(),
                                                       &ecs::vfxModule(),     &ecs::levelModule(),  &ecs::worldRenderModule()};
    return all;
}

void registerTypes(reflect::TypeRegistry& types) { ecs::registerModuleTypes(types, modules()); }

void install(ecs::World& world, const ecs::Services& services, std::initializer_list<std::string_view> only) {
    if (only.size() == 0) return ecs::installModules(world, modules(), services);
    const auto all = modules();
    const auto find = [&](std::string_view name) {
        const auto m = std::find_if(all.begin(), all.end(), [&](const ecs::Module* x) { return x->name() == name; });
        if (m == all.end()) throw std::logic_error("engine::install: no engine module is named '" + std::string(name) + "'");
        return *m;
    };
    // The named modules and everything they depend on, with the core.
    std::vector<const ecs::Module*> chosen{&ecs::coreModule()};
    std::vector<const ecs::Module*> todo;
    for (const std::string_view name : only) todo.push_back(find(name));
    while (!todo.empty()) {
        const ecs::Module* m = todo.back();
        todo.pop_back();
        if (std::find(chosen.begin(), chosen.end(), m) != chosen.end()) continue;
        chosen.push_back(m);
        for (const std::string_view d : m->dependencies()) todo.push_back(find(d));
    }
    ecs::installModules(world, chosen, services);
}

} // namespace ohao::engine
