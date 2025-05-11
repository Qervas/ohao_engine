#pragma once
#include <memory>
#include <string>
#include <typeinfo>
#include <typeindex>
#include <functional>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace ohao {

class Actor; // Forward declaration
class Scene;

class Component {
public:
    using Ptr = std::shared_ptr<Component>;
    
    Component(Actor* owner = nullptr);
    virtual ~Component() = default;
    
    // Lifecycle methods
    virtual void initialize() {}
    virtual void start() {}
    virtual void update(float deltaTime) {}
    virtual void render() {}
    virtual void destroy() {}
    
    // Serialization interface
    virtual nlohmann::json serialize() const = 0;
    virtual void deserialize(const nlohmann::json& data) = 0;
    
    // Owner management
    void setOwner(Actor* newOwner) { owner = newOwner; }
    Actor* getOwner() const { return owner; }
    Scene* getScene() const;
    
    // Enable/disable the component
    void setEnabled(bool enabled);
    bool isEnabled() const;
    
    // Type information
    virtual const char* getTypeName() const = 0;
    static const std::string& getStaticTypeName();
    std::type_index getTypeIndex() const;
    
    // Unique ID for component instance
    std::uint64_t getID() const { return componentID; }
    
    // Change tracking
    virtual void beginModification();
    virtual void endModification();
    virtual bool isModified() const { return modified; }
    virtual void clearModified() { modified = false; }
    
protected:
    // Called when component is attached/detached from an actor
    virtual void onAttach() {}
    virtual void onDetach() {}
    virtual void onUpdate(float deltaTime) {}
    
    Actor* owner;
    bool enabled;
    std::uint64_t componentID;
    bool modified;
    nlohmann::json oldState;

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