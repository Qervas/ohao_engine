#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>
#include <type_traits>
#include <typeindex>
#include <atomic>
#include <algorithm> // Required for std::find
#include "../scene/scene_object.hpp" // Include SceneObject
#include "../component/component.hpp" // Include Component
#include "../component/transform_component.hpp" // Include TransformComponent directly

// Forward declarations
namespace ohao {
class Scene;
class Component;
class Model;
class Material;

// Define ObjectID type for compatibility with SceneObject
using ObjectID = uint64_t;

class Actor : public SceneObject {
public:
    using Ptr = std::shared_ptr<Actor>;

    Actor(const std::string& name = "Actor");
    ~Actor() override;

    // Scene management
    void setScene(Scene* scene);
    Scene* getScene() const { return scene; }

    // Lifecycle methods
    virtual void initialize();
    virtual void start();
    virtual void update(float deltaTime) override;
    virtual void render();
    virtual void destroy();

    // Hierarchy management (now manages Actor* instead of SceneNode*)
    void setParent(Actor* parent);
    Actor* getParent() const { return parent; }
    void addChild(Actor* child);
    void removeChild(Actor* child);
    const std::vector<Actor*>& getChildren() const { return children; }
    void detachFromParent();

    // Name properties are inherited from SceneObject
    const std::string& getName() const { return SceneObject::getName(); }
    void setName(const std::string& newName) { SceneObject::setName(newName); }

    // Active state
    bool isActive() const { return active; }
    void setActive(bool isActive) { active = isActive; }

    // Transform helpers
    TransformComponent* getTransform() const;
    
    // Transform compatibility with SceneObject
    glm::mat4 getWorldMatrix() const { 
        return getWorldTransform();
    }

    // Unique ID is inherited from SceneObject
    ObjectID getID() const { return SceneObject::getID(); }

    // Component management - implemented inline for simplicity
    template<typename T, typename... Args>
    std::shared_ptr<T> addComponent(Args&&... args) {
        static_assert(std::is_base_of<Component, T>::value, "Type must be a Component");
        
        // Create new component with provided arguments
        std::shared_ptr<T> component = std::make_shared<T>(std::forward<Args>(args)...);
        
        // Set this actor as the owner
        component->setOwner(this);
        
        // Add to component lists
        components.push_back(component);
        componentsByType[std::type_index(typeid(T))] = component;
        
        // Initialize the component if actor is already active
        if (isActive()) {
            component->initialize();
        }
        
        // Notify deriving classes
        onComponentAdded(component);
        
        return component;
    }

    template<typename T>
    std::shared_ptr<T> getComponent() const {
        static_assert(std::is_base_of<Component, T>::value, "Type must be a Component");
        
        // Look for the component type in our map
        auto it = componentsByType.find(std::type_index(typeid(T)));
        if (it != componentsByType.end()) {
            // Cast the component to the requested type
            return std::static_pointer_cast<T>(it->second);
        }
        
        return nullptr;
    }

    template<typename T>
    bool hasComponent() const {
        static_assert(std::is_base_of<Component, T>::value, "Type must be a Component");
        
        // Look for the component type in our map
        auto it = componentsByType.find(std::type_index(typeid(T)));
        return it != componentsByType.end();
    }

    template<typename T>
    bool removeComponent() {
        static_assert(std::is_base_of<Component, T>::value, "Type must be a Component");
        
        // Look for the component type in our map
        auto it = componentsByType.find(std::type_index(typeid(T)));
        if (it != componentsByType.end()) {
            // Get the component
            std::shared_ptr<Component> component = it->second;
            
            // Remove from type map
            componentsByType.erase(it);
            
            // Remove from components vector using std::find
            auto compIt = std::find_if(components.begin(), components.end(), 
                [&component](const std::shared_ptr<Component>& comp) {
                    return comp == component;
                });
                
            if (compIt != components.end()) {
                components.erase(compIt);
            }
            
            // Clean up component
            component->destroy();
            component->setOwner(nullptr);
            
            // Notify deriving classes
            onComponentRemoved(component);
            
            return true;
        }
        
        return false;
    }

    void removeAllComponents();

    const std::vector<std::shared_ptr<Component>>& getAllComponents() const { return components; }

    // SceneObject compatibility methods are already inherited
    // These will be implemented in actor.cpp for component integration
    void setModel(std::shared_ptr<Model> model);
    std::shared_ptr<Model> getModel() const;
    void setMaterial(const Material& material);
    const Material& getMaterial() const;
    Material& getMaterial();
    
    // Type information override
    const char* getTypeName() const override { return "Actor"; }

protected:
    // Called when a component is added to this actor
    virtual void onComponentAdded(std::shared_ptr<Component> component);
    
    // Called when a component is removed from this actor
    virtual void onComponentRemoved(std::shared_ptr<Component> component);
    
    // Called when this actor is added to a scene
    void onAddedToScene() override;
    
    // Called when this actor is removed from a scene
    void onRemovedFromScene() override;

    Scene* scene;
    Actor* parent;
    bool active;
    std::vector<Actor*> children;
    std::vector<std::shared_ptr<Component>> components;
    std::unordered_map<std::type_index, std::shared_ptr<Component>> componentsByType;
};

} // namespace ohao 