#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>
#include <typeindex>
#include <atomic>
#include <algorithm> // Required for std::find
#include "core/concepts.hpp"
#include "scene/scene_object.hpp" // Include SceneObject
#include "scene/component/component.hpp" // Include Component
#include "scene/component/transform_component.hpp" // Include TransformComponent directly

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

    explicit Actor(std::string_view name = "Actor");
    ~Actor() override;

    // Scene management
    void setScene(Scene* scene);
    [[nodiscard]] Scene* getScene() const { return scene; }

    // Lifecycle methods
    virtual void initialize();
    virtual void start();
    virtual void update(float deltaTime);
    virtual void render();
    virtual void destroy();

    // Hierarchy management (now manages Actor* instead of SceneNode*)
    void setParent(Actor* parent);
    [[nodiscard]] Actor* getParent() const { return parent; }
    void addChild(Actor* child);
    void removeChild(Actor* child);
    [[nodiscard]] const std::vector<Actor*>& getChildren() const { return children; }
    void detachFromParent();

    // Name properties are inherited from SceneObject
    [[nodiscard]] const std::string& getName() const { return SceneObject::getName(); }
    void setName(std::string_view newName) { SceneObject::setName(std::string(newName)); }

    // Active state
    [[nodiscard]] bool isActive() const { return active; }
    void setActive(bool isActive) { active = isActive; }

    // Editor visibility (for viewport display, like Blender's eye icon)
    [[nodiscard]] bool isEditorVisible() const { return editorVisible; }
    void setEditorVisible(bool visible) { editorVisible = visible; }

    // Transform helpers
    [[nodiscard]] TransformComponent* getTransform() const;
    
    // Transform compatibility with SceneObject
    [[nodiscard]] glm::mat4 getWorldMatrix() const { 
        return getTransform() ? getTransform()->getWorldMatrix() : glm::mat4(1.0f);
    }

    // Unique ID is inherited from SceneObject
    [[nodiscard]] ObjectID getID() const { return SceneObject::getID(); }

    // Stable GUID for map serialization
    [[nodiscard]] const std::string& getGuid() const { return guid; }
    void setGuid(std::string_view g) { guid = std::string(g); }

    // Tags (scene queries via Scene::findActorsByTag)
    void addTag(std::string_view tag);
    void removeTag(std::string_view tag);
    [[nodiscard]] bool hasTag(std::string_view tag) const;
    [[nodiscard]] const std::vector<std::string>& getTags() const noexcept { return tags; }
    void clearTags() noexcept { tags.clear(); }

    // Component management - implemented inline for simplicity
    template<ComponentType T, typename... Args>
    std::shared_ptr<T> addComponent(Args&&... args) {
        std::shared_ptr<T> component = std::make_shared<T>(std::forward<Args>(args)...);
        component->setOwner(this);
        components.push_back(component);
        componentsByType[std::type_index(typeid(T))] = component;
        if (isActive()) { component->initialize(); }
        onComponentAdded(component);
        return component;
    }

    template<ComponentType T>
    [[nodiscard]] std::shared_ptr<T> getComponent() const {
        auto it = componentsByType.find(std::type_index(typeid(T)));
        if (it != componentsByType.end()) {
            return std::static_pointer_cast<T>(it->second);
        }
        return nullptr;
    }

    template<ComponentType T>
    [[nodiscard]] bool hasComponent() const {
        return componentsByType.contains(std::type_index(typeid(T)));
    }

    template<ComponentType T>
    bool removeComponent() {
        auto it = componentsByType.find(std::type_index(typeid(T)));
        if (it == componentsByType.end()) return false;

        std::shared_ptr<Component> component = it->second;
        componentsByType.erase(it);
        std::erase_if(components, [&](const std::shared_ptr<Component>& c) {
            return c == component;
        });
        component->destroy();
        component->setOwner(nullptr);
        onComponentRemoved(component);
        return true;
    }

    void removeAllComponents();

    [[nodiscard]] const std::vector<std::shared_ptr<Component>>& getAllComponents() const noexcept {
        return components;
    }

    [[nodiscard]] std::size_t componentCount() const noexcept { return components.size(); }

    /// Visit every component (non-hot-path).
    template<typename F>
        requires std::invocable<F&, Component&>
    void forEachComponent(F&& fn) {
        for (auto& c : components) {
            if (c) fn(*c);
        }
    }

    void setModel(std::shared_ptr<Model> model);
    [[nodiscard]] std::shared_ptr<Model> getModel() const;
    void setMaterial(const Material& material);
    [[nodiscard]] const Material& getMaterial() const;
    [[nodiscard]] Material& getMaterial();
    
    [[nodiscard]] const char* getTypeName() const override { return "Actor"; }

protected:
    virtual void onComponentAdded(std::shared_ptr<Component> component);
    virtual void onComponentRemoved(std::shared_ptr<Component> component);
    void onAddedToScene();
    void onRemovedFromScene();

    Scene* scene;
    Actor* parent;
    bool active;
    bool editorVisible{true};  // Viewport visibility (eye icon in outliner)
    std::vector<Actor*> children;
    std::vector<std::shared_ptr<Component>> components;
    std::unordered_map<std::type_index, std::shared_ptr<Component>> componentsByType;
    std::string guid;
    std::vector<std::string> tags;
};

} // namespace ohao 