#pragma once
#include "core/concepts.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <typeindex>
#include <functional>
#include <cstdint>

namespace ohao {

class Actor; // Forward declaration

class Component {
public:
    using Ptr = std::shared_ptr<Component>;
    
    Component();
    virtual ~Component() = default;
    
    // Lifecycle methods
    virtual void initialize() {}
    virtual void start() {}
    virtual void update(float deltaTime) {}
    virtual void render() {}
    virtual void destroy() {}
    
    // Owner management
    void setOwner(Actor* owner);
    [[nodiscard]] Actor* getOwner() const;
    
    // Enable/disable the component
    void setEnabled(bool enabled);
    [[nodiscard]] bool isEnabled() const;
    
    // Type information
    [[nodiscard]] virtual const char* getTypeName() const;
    [[nodiscard]] std::type_index getTypeIndex() const;
    
    // Unique ID for component instance
    [[nodiscard]] std::uint64_t getID() const { return componentID; }

    // Stable GUID for map serialization
    [[nodiscard]] const std::string& getGuid() const { return guid; }
    void setGuid(std::string_view g) { guid = std::string(g); }
    
protected:
    virtual void onAttached() {}
    virtual void onDetached() {}
    
    Actor* owner;
    bool enabled;
    std::uint64_t componentID;
    std::string guid;

private:
    static std::uint64_t nextComponentID;
};

// Template function to check component type
template<ComponentType T>
[[nodiscard]] bool isComponentType(const Component* component) {
    return component != nullptr && typeid(*component) == typeid(T);
}

// Template function to cast component to derived type
template<ComponentType T>
[[nodiscard]] T* componentCast(Component* component) {
    return isComponentType<T>(component) ? static_cast<T*>(component) : nullptr;
}

} // namespace ohao
