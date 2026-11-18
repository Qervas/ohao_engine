#pragma once
// Modules (step 4b-6b; docs/specs/2026-11-18-engine-modules-design.md, section 4): one interface for the engine's
// subsystems and for games. A module is one directory and one library (cmake/ohao_modules.cmake): its components and
// their registrations, its events, its systems and their install, its API methods. It depends only on the modules it
// declares (CMake's DEPENDS, the one source of the graph; tools/check_layers.py refuses an include of any other's
// header); everything else between modules is components and events in the World.
//
// A host registers every module's types (registerModuleTypes) before a scene loads, and installs them on each World
// (installModules) in dependency order, ties to the lesser name.
#include "core/type_key.hpp"
#include "reflect/schema.hpp"
#include "reflect/type_info.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao::reflect {
class DecodeContext;
class JsonWriter;
class TypeRegistry;
struct JsonNode;
} // namespace ohao::reflect

namespace ohao::ecs {

class World;
class AssetResolver;

// What a module's method is given.
struct Call {
    World& world;
    bool paramsFailed{false};   // the reported errors are about the params (the caller's mistake)
};

// Decodes a method's params into `out`, a `t` (from the module's own registry): unknown members, types, enums and
// limits as scene files report them, then the required members. False after reporting, with call.paramsFailed set.
// Inline: a game calls it, and the host executable need not carry it (game/loader.hpp).
inline bool decodeParams(Call& call, const reflect::TypeInfo& t, const reflect::JsonNode& params,
                         reflect::DecodeContext& ctx, void* out) {
    if (reflect::readParams(out, t, params, ctx)) return true;
    call.paramsFailed = true;
    return false;
}

// A method writes its result value into `out`, or reports into ctx and returns false.
using MethodFn = std::function<bool(Call&, const reflect::JsonNode& params, reflect::DecodeContext& ctx,
                                    reflect::JsonWriter& out)>;

struct Method {
    reflect::MethodDoc doc;   // its row in the catalogue and in MCP's tools (params: the module's registry's type)
    MethodFn fn;
    bool mutates{false};      // may change the world
};

// What a host gives the modules: the assets and the types always; anything else by type (the renderer's scene and
// textures, once a host has them). The core names no renderer type.
class Services {
public:
    Services(AssetResolver& assets, const reflect::TypeRegistry& types) : assets(assets), types(types) {}

    AssetResolver& assets;
    const reflect::TypeRegistry& types;

    template <class T>
    void put(T& service) {
        for (auto& [key, ptr] : m_services)
            if (key == typeKey<T>()) {
                ptr = &service;
                return;
            }
        m_services.emplace_back(typeKey<T>(), &service);
    }
    // Null when the host has none.
    template <class T>
    [[nodiscard]] T* get() const {
        for (const auto& [key, ptr] : m_services)
            if (key == typeKey<T>()) return static_cast<T*>(ptr);
        return nullptr;
    }
    [[nodiscard]] bool has(TypeKey key) const noexcept {
        for (const auto& [k, ptr] : m_services)
            if (k == key) return true;
        return false;
    }

private:
    std::vector<std::pair<TypeKey, void*>> m_services;
};

class Module {
public:
    virtual ~Module() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    // An engine module's: generated from its CMake DEPENDS (generated/modules/<name>_dependencies.hpp). A game's: the
    // engine modules it uses.
    [[nodiscard]] virtual std::span<const std::string_view> dependencies() const { return {}; }
    // Its components and event payloads, before any scene loads (registerModuleTypes records its name on each).
    virtual void registerTypes(reflect::TypeRegistry& types) const { (void)types; }
    // The services it needs beyond the assets and the types (by typeKey); installModules installs it at the first
    // call whose services have them all.
    [[nodiscard]] virtual std::span<const TypeKey> needs() const { return {}; }
    // Its systems, channels, hooks and rules on `world`, once per World (installModules never installs it twice).
    virtual void install(World& world, const Services& services) const = 0;
    // What install made that the engine cannot see (a game's context entries, its registry signals); engine modules
    // are never uninstalled.
    virtual void uninstall(World& world) const { (void)world; }
    // Its API methods, the session's catalogue rows.
    [[nodiscard]] virtual std::span<const Method> methods() const { return {}; }
    // What other modules (a game) added to this one on `world`, by name (physics: the control rule), and their
    // removal: what game::uninstallGame takes back.
    [[nodiscard]] virtual std::vector<std::string> contributions(const World& world) const {
        (void)world;
        return {};
    }
    virtual void removeContributions(World& world, std::span<const std::string> names) const {
        (void)world;
        (void)names;
    }
};

// Each module after its dependencies, ties to the lesser name. A dependency that is not among `modules`, or a cycle,
// throws std::logic_error naming them.
[[nodiscard]] std::vector<const Module*> moduleOrder(std::span<const Module* const> modules);
// Every module's types, in module order, each under TypeRegistry::setModule(its name).
void registerModuleTypes(reflect::TypeRegistry& types, std::span<const Module* const> modules);
// Installs, in module order, each module not yet installed on `world` whose needs `services` meets, and records it
// and the services in the World (a game installed later gets them: game::installGame). A dependency outside
// `modules` is taken as met: a module's systems never need another's to be installed (its types are registered by
// every host), so a bench installs only what it measures. Between steps.
void installModules(World& world, std::span<const Module* const> modules, const Services& services);
// The modules installed on `world`, in the order installed.
[[nodiscard]] std::span<const Module* const> installedModules(const World& world);
// The services the World's modules were installed with (the last installModules call's), or null.
[[nodiscard]] const Services* worldServices(const World& world);

// The core module, "ecs" (ecs/core.cpp): the World's own types (ids, names, the hierarchy, transforms, input, spawn
// points, spin and fly).
[[nodiscard]] const Module& coreModule();
// The core's registrations (ecs/types.cpp): its components and the World's event payload.
void registerCoreTypes(reflect::TypeRegistry& types);

} // namespace ohao::ecs
