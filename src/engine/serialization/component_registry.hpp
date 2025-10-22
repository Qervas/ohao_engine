#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <nlohmann/json_fwd.hpp>

namespace ohao {

class Actor;
class Component;

struct ComponentEntry {
    std::function<Component*(Actor*)> create;
    std::function<nlohmann::json_abi_v3_11_3::json(const Component&)> serialize;
    std::function<void(Component&, const nlohmann::json_abi_v3_11_3::json&)> deserialize;
};

class ComponentRegistry {
public:
    static ComponentRegistry& get();

    void registerComponent(const std::string& typeName, ComponentEntry entry);
    const ComponentEntry* find(const std::string& typeName) const;

private:
    std::unordered_map<std::string, ComponentEntry> entries;
};

#define OHAO_REGISTER_COMPONENT(Type, NameStr, CreateFn, SerializeFn, DeserializeFn) \
    namespace { \
    struct Type##RegistryHook { \
        Type##RegistryHook(){ \
            ComponentRegistry::get().registerComponent(NameStr, {CreateFn, SerializeFn, DeserializeFn}); \
        } \
    }; \
    static Type##RegistryHook g_##Type##RegistryHook; \
    }

} // namespace ohao


