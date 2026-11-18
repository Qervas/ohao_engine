#include "ecs/module.hpp"

#include "ecs/world.hpp"
#include "reflect/registry.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace ohao::ecs {

namespace {

// What installModules put on a World.
struct InstalledModules {
    std::vector<const Module*> modules;   // in the order installed
    std::optional<Services> services;     // the last call's
};

const Module* byName(std::span<const Module* const> modules, std::string_view name) {
    for (const Module* m : modules)
        if (m->name() == name) return m;
    return nullptr;
}

} // namespace

std::vector<const Module*> moduleOrder(std::span<const Module* const> modules) {
    for (const Module* m : modules)
        for (const std::string_view d : m->dependencies())
            if (!byName(modules, d))
                throw std::logic_error("module '" + std::string(m->name()) + "' depends on '" + std::string(d) +
                                       "', which is not among the modules given");
    // Kahn's algorithm, the least name first among the ready.
    std::vector<const Module*> left(modules.begin(), modules.end());
    std::sort(left.begin(), left.end(), [](const Module* a, const Module* b) { return a->name() < b->name(); });
    std::vector<const Module*> out;
    while (!left.empty()) {
        const auto ready = std::find_if(left.begin(), left.end(), [&](const Module* m) {
            const auto deps = m->dependencies();
            return std::all_of(deps.begin(), deps.end(), [&](std::string_view d) { return byName(out, d) != nullptr; });
        });
        if (ready == left.end()) {
            std::string names;
            for (const Module* m : left) names += (names.empty() ? "" : ", ") + std::string(m->name());
            throw std::logic_error("the modules " + names + " depend on each other in a cycle");
        }
        out.push_back(*ready);
        left.erase(ready);
    }
    return out;
}

std::vector<const Module*> withDependencies(std::span<const Module* const> all, std::span<const std::string_view> names) {
    std::vector<const Module*> out;
    std::vector<std::string_view> todo(names.begin(), names.end());
    while (!todo.empty()) {
        const std::string_view n = todo.back();
        todo.pop_back();
        if (byName(out, n)) continue;
        const Module* m = byName(all, n);
        if (!m) throw std::logic_error("no engine module is named '" + std::string(n) + "'");
        out.push_back(m);
        for (const std::string_view d : m->dependencies()) todo.push_back(d);
    }
    return out;
}

void registerModuleTypes(reflect::TypeRegistry& types, std::span<const Module* const> modules) {
    for (const Module* m : moduleOrder(modules)) {
        types.setModule(std::string(m->name()));
        m->registerTypes(types);
    }
    types.setModule({});
}

void installModules(World& world, std::span<const Module* const> modules, const Services& services) {
    auto& ctx = world.context();
    InstalledModules& rec = ctx.contains<InstalledModules>() ? ctx.get<InstalledModules>() : ctx.emplace<InstalledModules>();
    rec.services.emplace(services);
    const auto installed = [&](std::string_view name) { return byName(rec.modules, name) != nullptr; };
    for (const Module* m : moduleOrder(modules)) {
        if (installed(m->name())) continue;
        const auto needs = m->needs();
        const auto deps = m->dependencies();
        // It waits for its needs, and for a dependency that waits for its own.
        if (!std::all_of(needs.begin(), needs.end(), [&](TypeKey k) { return services.has(k); }) ||
            !std::all_of(deps.begin(), deps.end(), installed))
            continue;
        m->install(world, *rec.services);
        rec.modules.push_back(m);
    }
}

std::span<const Module* const> installedModules(const World& world) {
    const auto* rec = world.context().find<InstalledModules>();
    return rec ? std::span<const Module* const>(rec->modules) : std::span<const Module* const>();
}

const Services* worldServices(const World& world) {
    const auto* rec = world.context().find<InstalledModules>();
    return rec && rec->services ? &*rec->services : nullptr;
}

} // namespace ohao::ecs
