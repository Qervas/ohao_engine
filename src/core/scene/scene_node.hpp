#pragma once
#include "transform.hpp"
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace ohao {

class SceneNode : public std::enable_shared_from_this<SceneNode> {
public:
    using Ptr = std::shared_ptr<SceneNode>;
    using WeakPtr = std::weak_ptr<SceneNode>;

    SceneNode(const std::string& name = "Node");
    virtual ~SceneNode() = default;

    // Hierarchy management
    void addChild(Ptr child);
    void removeChild(Ptr child);
    void setParent(Ptr parent);
    void detachFromParent();

    // Tree traversal
    SceneNode* findChild(const std::string& name);
    std::vector<SceneNode*> findChildren(const std::string& name);

    // Hierarchy queries
    bool isAncestorOf(const SceneNode* node) const;
    bool isDescendantOf(const SceneNode* node) const;

    // Getters
    const std::string& getName() const { return name; }
    Transform& getTransform() { return transform; }
    const Transform& getTransform() const { return transform; }
    SceneNode* getParent() const { return parent.lock().get(); }
    const std::vector<Ptr>& getChildren() const { return children; }

    // Setters
    void setName(const std::string& newName) { name = newName; }
    void setEnabled(bool value) { enabled = value; }
    bool isEnabled() const { return enabled; }

    // Virtual update method for derived classes
    virtual void update(float deltaTime);

protected:
    std::string name;
    Transform transform;
    bool enabled{true};

    WeakPtr parent;
    std::vector<Ptr> children;

    virtual void onAddedToScene();
    virtual void onRemovedFromScene();
};

} // namespace ohao
