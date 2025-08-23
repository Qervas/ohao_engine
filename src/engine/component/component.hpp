#pragma once
#include <memory>
#include <string>
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
    
    // Serialization interface
    virtual void serialize(class Serializer& serializer) const {}
    virtual void deserialize(class Deserializer& deserializer) {}
    
    // Owner management
    void setOwner(Actor* owner);
    Actor* getOwner() const;
    
    // Enable/disable the component
    void setEnabled(bool enabled);
    bool isEnabled() const;
    
    // Type information
    virtual const char* getTypeName() const;
    std::type_index getTypeIndex() const;
    
    // Unique ID for component instance
    std::uint64_t getID() const { return componentID; }
    
protected:
    // Called when component is attached/detached from an actor
    virtual void onAttached() {}
    virtual void onDetached() {}
    
    Actor* owner;
    bool enabled;
    std::uint64_t componentID;

private:
    static std::uint64_t nextComponentID;
};

// Template function to check component type
template<typename T>
bool isComponentType(const Component* component) {
    static_assert(std::is_base_of<Component, T>::value, "Type must be a Component");
    return typeid(*component) == typeid(T);
}

// Template function to cast component to derived type
template<typename T>
T* componentCast(Component* component) {
    static_assert(std::is_base_of<Component, T>::value, "Type must be a Component");
    return (isComponentType<T>(component)) ? static_cast<T*>(component) : nullptr;
}

} // namespace ohao 