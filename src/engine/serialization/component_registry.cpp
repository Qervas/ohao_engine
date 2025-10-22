#include "component_registry.hpp"

namespace ohao {

ComponentRegistry& ComponentRegistry::get(){ static ComponentRegistry inst; return inst; }

void ComponentRegistry::registerComponent(const std::string& typeName, ComponentEntry entry){
    entries[typeName] = std::move(entry);
}

const ComponentEntry* ComponentRegistry::find(const std::string& typeName) const{
    auto it = entries.find(typeName);
    if(it==entries.end()) return nullptr; return &it->second;
}

} // namespace ohao


